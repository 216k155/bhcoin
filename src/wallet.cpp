// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The LUX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet.h"

#include "base58.h"
#include "checkpoints.h"
#include "coincontrol.h"
#include "consensus/validation.h"
#include "stake.h"
#include "net.h"
#include "main.h"
#include "script/script.h"
#include "script/sign.h"
#include "spork.h"
#include "instantx.h"
#include "timedata.h"
#include "txmempool.h"
#include "util.h"
#include "ui_interface.h"
#include "utilmoneystr.h"
#include "script/interpreter.h"

#include <assert.h>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

#define SELECT_COINS_FROM_ACCOUNT true

#if defined(DEBUG_DUMP_STAKING_INFO)
#include "DEBUG_DUMP_STAKING_INFO.hpp"
#endif

using namespace std;

/**
 * Settings
 */
CFeeRate payTxFee(DEFAULT_TRANSACTION_FEE);
CAmount maxTxFee = DEFAULT_TRANSACTION_MAXFEE;
unsigned int nTxConfirmTarget = 1;
bool bSpendZeroConfChange = true;
bool fSendFreeTransactions = false;
bool fPayAtLeastCustomFee = true;
OutputType g_address_type = OUTPUT_TYPE_NONE;
OutputType g_change_type = OUTPUT_TYPE_NONE;
const uint32_t BIP32_HARDENED_KEY_LIMIT = 0x80000000;
const char * DEFAULT_WALLET_DAT = "wallet.dat";

bool bZeroBalanceAddressToken = DEFAULT_ZERO_BALANCE_ADDRESS_TOKEN;
bool fNotUseChangeAddress = DEFAULT_NOT_USE_CHANGE_ADDRESS;
/**
 * Fees smaller than this (in duffs) are considered zero fee (for transaction creation)
 * We are ~100 times smaller then bitcoin now (2015-06-23), set minTxFee 10 times higher
 * so it's still 10 times lower comparing to bitcoin.
 * Override with -mintxfee
 */
CFeeRate CWallet::minTxFee = CFeeRate(10000);

const uint256 CMerkleTx::ABANDON_HASH(uint256S("0000000000000000000000000000000000000000000000000000000000000001"));

/** @defgroup mapWallet
 *
 * @{
 */

struct CompareValueOnly {
    bool operator()(const std::pair<CAmount, std::pair<const CWalletTx*, unsigned int> >& t1,
                    const std::pair<CAmount, std::pair<const CWalletTx*, unsigned int> >& t2) const
    {
        return t1.first < t2.first;
    }
};

std::string COutput::ToString() const
{
    return strprintf("COutput(%s, %d, %d) [%s]", tx->GetHash().ToString(), i, nDepth, FormatMoney(tx->tx->vout[i].nValue));
}

class CAffectedKeysVisitor : public boost::static_visitor<void> {
private:
    const CKeyStore &keystore;
    std::vector<CKeyID> &vKeys;

public:
    CAffectedKeysVisitor(const CKeyStore &keystoreIn, std::vector<CKeyID> &vKeysIn) : keystore(keystoreIn), vKeys(vKeysIn) {}

    void Process(const CScript &script) {
        txnouttype type;
        std::vector<CTxDestination> vDest;
        int nRequired;
        if (ExtractDestinations(script, type, vDest, nRequired)) {
            for (const CTxDestination &dest : vDest)
                boost::apply_visitor(*this, dest);
        }
    }

    void operator()(const CKeyID &keyId) {
        if (keystore.HaveKey(keyId))
            vKeys.push_back(keyId);
    }

    void operator()(const CScriptID &scriptId) {
        CScript script;
        if (keystore.GetCScript(scriptId, script))
            Process(script);
    }

    void operator()(const WitnessV0ScriptHash& scriptID)
    {
        CScriptID id;
        CRIPEMD160().Write(scriptID.begin(), 32).Finalize(id.begin());
        CScript script;
        if (keystore.GetCScript(id, script)) {
            Process(script);
        }
    }

    void operator()(const WitnessV0KeyHash& keyid)
    {
        CKeyID id(keyid);
        if (keystore.HaveKey(id)) {
            vKeys.push_back(id);
        }
    }

    template<typename X>
    void operator()(const X &none) {}
};

const CWalletTx* CWallet::GetWalletTx(const uint256& hash) const
{
    LOCK(cs_wallet);
    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(hash);
    if (it == mapWallet.end())
        return nullptr;
    return &(it->second);
}

CPubKey CWallet::GenerateNewKey(CWalletDB &walletdb, bool internal)
{
    AssertLockHeld(cs_wallet);                                 // mapKeyMetadata
    bool fCompressed = CanSupportFeature(FEATURE_COMPRPUBKEY); // default to compressed public keys if we want 0.6.0 wallets

    CKey secret;

    // Create new metadata
    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    // use HD key derivation if HD was enabled during wallet creation
    if (IsHDEnabled()) {
        DeriveNewChildKey(walletdb, metadata, secret, (CanSupportFeature(FEATURE_HD_SPLIT) ? internal : false));
    } else {
        secret.MakeNewKey(fCompressed);
    }

    // Compressed public keys were introduced in version 0.6.0
    if (fCompressed) {
        SetMinVersion(FEATURE_COMPRPUBKEY);
    }

    CPubKey pubkey = secret.GetPubKey();
    assert(secret.VerifyPubKey(pubkey));

    mapKeyMetadata[pubkey.GetID()] = metadata;
    UpdateTimeFirstKey(nCreationTime);

    if (!AddKeyPubKeyWithDB(walletdb, secret, pubkey)) {
        throw std::runtime_error(std::string(__func__) + ": AddKey failed");
    }
    return pubkey;
}

void CWallet::DeriveNewChildKey(CWalletDB &walletdb, CKeyMetadata& metadata, CKey& secret, bool internal)
{
    // for now we use a fixed keypath scheme of m/88'/0'/k
    CKey key;                      //master key seed (256bit)
    CExtKey masterKey;             //hd master key
    CExtKey accountKey;            //key at m/88'
    CExtKey chainChildKey;         //key at m/88'/0' (external) or m/0'/1' (internal)
    CExtKey childKey;              //key at m/88'/0'/<n>'

    // try to get the master key
    if (!GetKey(hdChain.masterKeyID, key))
        throw std::runtime_error(std::string(__func__) + ": Master key not found");

    masterKey.SetMaster(key.begin(), key.size());

    // derive m/0'
    // use hardened derivation (child keys >= 0x80000000 are hardened after bip32)
    masterKey.Derive(accountKey, BIP32_HARDENED_KEY_LIMIT);

    // derive m/0'/0' (external chain) OR m/0'/1' (internal chain)
    assert(internal ? CanSupportFeature(FEATURE_HD_SPLIT) : true);
    accountKey.Derive(chainChildKey, BIP32_HARDENED_KEY_LIMIT+(internal ? 1 : 0));

    // derive child key at next index, skip keys already known to the wallet
    do {
        // always derive hardened keys
        // childIndex | BIP32_HARDENED_KEY_LIMIT = derive childIndex in hardened child-index-range
        // example: 1 | BIP32_HARDENED_KEY_LIMIT == 0x80000001 == 2147483649
        if (internal) {
            chainChildKey.Derive(childKey, hdChain.nInternalChainCounter | BIP32_HARDENED_KEY_LIMIT);
            metadata.hdKeypath = "m/88'/1'/" + std::to_string(hdChain.nInternalChainCounter) + "'";
            hdChain.nInternalChainCounter++;
        }
        else {
            chainChildKey.Derive(childKey, hdChain.nExternalChainCounter | BIP32_HARDENED_KEY_LIMIT);
            metadata.hdKeypath = "m/88'/0'/" + std::to_string(hdChain.nExternalChainCounter) + "'";
            hdChain.nExternalChainCounter++;
        }
    } while (HaveKey(childKey.key.GetPubKey().GetID()));
    secret = childKey.key;
    metadata.hdMasterKeyID = hdChain.masterKeyID;
    // update the chain model in the database
    if (!walletdb.WriteHDChain(hdChain))
        throw std::runtime_error(std::string(__func__) + ": Writing HD chain model failed");
}

bool CWallet::AddKeyPubKeyWithDB(CWalletDB &walletdb, const CKey& secret, const CPubKey &pubkey)
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata

    // CCryptoKeyStore has no concept of wallet databases, but calls AddCryptedKey
    // which is overridden below.  To avoid flushes, the database handle is
    // tunneled through to it.
    bool needsDB = !pwalletdbEncryption;
    if (needsDB) {
        pwalletdbEncryption = &walletdb;
    }
    if (!CCryptoKeyStore::AddKeyPubKey(secret, pubkey)) {
        if (needsDB) pwalletdbEncryption = nullptr;
        return false;
    }
    if (needsDB) pwalletdbEncryption = nullptr;

    // check if we need to remove from watch-only
    CScript script;
    script = GetScriptForDestination(pubkey.GetID());
    if (HaveWatchOnly(script)) {
        RemoveWatchOnly(script);
    }
    script = GetScriptForRawPubKey(pubkey);
    if (HaveWatchOnly(script)) {
        RemoveWatchOnly(script);
    }

    if (!IsCrypted()) {
        return walletdb.WriteKey(pubkey,
                                                 secret.GetPrivKey(),
                                                 mapKeyMetadata[pubkey.GetID()]);
    }
    return true;
}

bool CWallet::AddKeyPubKey(const CKey& secret, const CPubKey &pubkey)
{
    CWalletDB walletdb(strWalletFile);
    return CWallet::AddKeyPubKeyWithDB(walletdb, secret, pubkey);
}

bool CWallet::AddCryptedKey(const CPubKey &vchPubKey,
                            const std::vector<unsigned char> &vchCryptedSecret)
{
    if (!CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret))
        return false;
    {
        LOCK(cs_wallet);
        if (pwalletdbEncryption)
            return pwalletdbEncryption->WriteCryptedKey(vchPubKey,
                vchCryptedSecret,
                mapKeyMetadata[vchPubKey.GetID()]);
        else
            return CWalletDB(strWalletFile).WriteCryptedKey(vchPubKey,
                                                            vchCryptedSecret,
                                                            mapKeyMetadata[vchPubKey.GetID()]);
    }
}

bool CWallet::LoadKeyMetadata(const CKeyID& keyID, const CKeyMetadata &meta)
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    UpdateTimeFirstKey(meta.nCreateTime);
    mapKeyMetadata[keyID] = meta;
    return true;
}

bool CWallet::LoadScriptMetadata(const CScriptID& script_id, const CKeyMetadata &meta)
{
    AssertLockHeld(cs_wallet); // m_script_metadata
    UpdateTimeFirstKey(meta.nCreateTime);
    m_script_metadata[script_id] = meta;
    return true;
}

bool CWallet::LoadCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret)
{
    return CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret);
}

void CWallet::UpdateTimeFirstKey(int64_t nCreateTime)
{
    AssertLockHeld(cs_wallet);
    if (nCreateTime <= 1) {
        // Cannot determine birthday information, so set the wallet birthday to
        // the beginning of time.
        nTimeFirstKey = 1;
    } else if (!nTimeFirstKey || nCreateTime < nTimeFirstKey) {
        nTimeFirstKey = nCreateTime;
    }
}

bool CWallet::AddCScript(const CScript& redeemScript)
{
    if (!CCryptoKeyStore::AddCScript(redeemScript))
        return false;
    return CWalletDB(strWalletFile).WriteCScript(Hash160(redeemScript), redeemScript);
}

bool CWallet::LoadCScript(const CScript& redeemScript)
{
    /* A sanity check was added in pull #3843 to avoid adding redeemScripts
     * that never can be redeemed. However, old wallets may still contain
     * these. Do not add them to the wallet and warn. */
    if (redeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE) {
        std::string strAddr = EncodeDestination(CScriptID(redeemScript));
        LogPrintf("%s: Warning: This wallet contains a redeemScript of size %i which exceeds maximum size %i thus can never be redeemed. Do not use address %s.\n",
            __func__, redeemScript.size(), MAX_SCRIPT_ELEMENT_SIZE, strAddr);
        return true;
    }

    return CCryptoKeyStore::AddCScript(redeemScript);
}

bool CWallet::AddWatchOnly(const CScript& dest)
{
    if (!CCryptoKeyStore::AddWatchOnly(dest))
        return false;
    const CKeyMetadata& meta = m_script_metadata[CScriptID(dest)];
    UpdateTimeFirstKey(meta.nCreateTime);
    NotifyWatchonlyChanged(true);
    return CWalletDB(strWalletFile).WriteWatchOnly(dest, meta);
}

bool CWallet::AddWatchOnly(const CScript& dest, int64_t nCreateTime)
{
    m_script_metadata[CScriptID(dest)].nCreateTime = nCreateTime;
    return AddWatchOnly(dest);
}

bool CWallet::RemoveWatchOnly(const CScript& dest)
{
    AssertLockHeld(cs_wallet);
    if (!CCryptoKeyStore::RemoveWatchOnly(dest))
        return false;
    if (!HaveWatchOnly())
        NotifyWatchonlyChanged(false);
    if (fFileBacked)
        if (!CWalletDB(strWalletFile).EraseWatchOnly(dest))
            return false;

    return true;
}

bool CWallet::LoadWatchOnly(const CScript& dest)
{
    return CCryptoKeyStore::AddWatchOnly(dest);
}

bool CWallet::Unlock(const SecureString& strWalletPassphrase, bool anonymizeOnly)
{
    SecureString strWalletPassphraseFinal;

    if (!IsLocked()) {
        fWalletUnlockAnonymizeOnly = anonymizeOnly;
        return true;
    }

    strWalletPassphraseFinal = strWalletPassphrase;


    CCrypter crypter;
    CKeyingMaterial _vMasterKey;

    {
        LOCK(cs_wallet);
        for (const MasterKeyMap::value_type& pMasterKey : mapMasterKeys) {
            if (!crypter.SetKeyFromPassphrase(strWalletPassphraseFinal, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, _vMasterKey))
                continue; // try another master key
            if (CCryptoKeyStore::Unlock(_vMasterKey)) {
                fWalletUnlockAnonymizeOnly = anonymizeOnly;
                return true;
            }
        }
    }
    return false;
}

bool CWallet::ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase)
{
    bool fWasLocked = IsLocked();
    SecureString strOldWalletPassphraseFinal = strOldWalletPassphrase;

    {
        LOCK(cs_wallet);
        Lock();

        CCrypter crypter;
        CKeyingMaterial _vMasterKey;
        for (MasterKeyMap::value_type& pMasterKey : mapMasterKeys) {
            if (!crypter.SetKeyFromPassphrase(strOldWalletPassphraseFinal, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, _vMasterKey))
                return false;
            if (CCryptoKeyStore::Unlock(_vMasterKey)) {
                int64_t nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = pMasterKey.second.nDeriveIterations * (100 / ((double)(GetTimeMillis() - nStartTime)));

                nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations + pMasterKey.second.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                LogPrintf("Wallet passphrase changed to an nDeriveIterations of %i\n", pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                    return false;
                if (!crypter.Encrypt(_vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;
                CWalletDB(strWalletFile).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    Lock();

                return true;
            }
        }
    }

    return false;
}

void CWallet::SetBestChain(const CBlockLocator& loc)
{
    CWalletDB walletdb(strWalletFile);
    walletdb.WriteBestBlock(loc);
}

bool CWallet::SetMinVersion(enum WalletFeature nVersion, CWalletDB* pwalletdbIn, bool fExplicit)
{
    LOCK(cs_wallet); // nWalletVersion
    if (nWalletVersion >= nVersion)
        return true;

    // when doing an explicit upgrade, if we pass the max version permitted, upgrade all the way
    if (fExplicit && nVersion > nWalletMaxVersion)
        nVersion = FEATURE_LATEST;

    nWalletVersion = nVersion;

    if (nVersion > nWalletMaxVersion)
        nWalletMaxVersion = nVersion;

    if (fFileBacked) {
        CWalletDB* pwalletdb = pwalletdbIn ? pwalletdbIn : new CWalletDB(strWalletFile);
        if (nWalletVersion > 40000)
            pwalletdb->WriteMinVersion(nWalletVersion);
        if (!pwalletdbIn)
            delete pwalletdb;
    }

    return true;
}

bool CWallet::SetMaxVersion(int nVersion)
{
    LOCK(cs_wallet); // nWalletVersion, nWalletMaxVersion
    // cannot downgrade below current version
    if (nWalletVersion > nVersion)
        return false;

    nWalletMaxVersion = nVersion;

    return true;
}

std::set<uint256> CWallet::GetConflicts(const uint256& txid) const
{
    std::set<uint256> result;
    AssertLockHeld(cs_wallet);

    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(txid);
    if (it == mapWallet.end())
        return result;
    const CWalletTx& wtx = it->second;

    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;

    for (const CTxIn& txin : wtx.tx->vin) {
        if (mapTxSpends.count(txin.prevout) <= 1)
            continue; // No conflict if zero or one spends
        range = mapTxSpends.equal_range(txin.prevout);
        for (TxSpends::const_iterator it = range.first; it != range.second; ++it)
            result.insert(it->second);
    }
    return result;
}

void CWallet::SyncMetaData(std::pair<TxSpends::iterator, TxSpends::iterator> range)
{
    // We want all the wallet transactions in range to have the same metadata as
    // the oldest (smallest nOrderPos).
    // So: find smallest nOrderPos:

    int nMinOrderPos = std::numeric_limits<int>::max();
    const CWalletTx* copyFrom = nullptr;
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const uint256& hash = it->second;
        int n = mapWallet[hash].nOrderPos;
        if (n < nMinOrderPos) {
            nMinOrderPos = n;
            copyFrom = &mapWallet[hash];
        }
    }
    // Now copy data from copyFrom to rest:
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const uint256& hash = it->second;
        CWalletTx* copyTo = &mapWallet[hash];
        if (copyFrom == copyTo) continue;
        //if (!copyFrom->IsEquivalentTo(*copyTo)) continue;
        copyTo->mapValue = copyFrom->mapValue;
        copyTo->vOrderForm = copyFrom->vOrderForm;
        // fTimeReceivedIsTxTime not copied on purpose
        // nTimeReceived not copied on purpose
        copyTo->nTimeSmart = copyFrom->nTimeSmart;
        copyTo->fFromMe = copyFrom->fFromMe;
        copyTo->strFromAccount = copyFrom->strFromAccount;
        // nOrderPos not copied on purpose
        // cached members not copied on purpose
    }
}

/**
 * Outpoint is spent if any non-conflicted transaction
 * spends it:
 */
bool CWallet::IsSpent(const uint256& hash, unsigned int n) const
{
    const COutPoint outpoint(hash, n);
    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;
    range = mapTxSpends.equal_range(outpoint);

    for (TxSpends::const_iterator it = range.first; it != range.second; ++it)
    {
        const uint256& wtxid = it->second;
        std::map<uint256, CWalletTx>::const_iterator mit = mapWallet.find(wtxid);
        if (mit != mapWallet.end()) {
            int depth = mit->second.GetDepthInMainChain();
            if (depth > 0  || (depth == 0 && !mit->second.isAbandoned()))
                return true; // Spent
        }
    }
    return false;
}

void CWallet::AddToSpends(const COutPoint& outpoint, const uint256& wtxid)
{
    mapTxSpends.insert(std::make_pair(outpoint, wtxid));

    std::pair<TxSpends::iterator, TxSpends::iterator> range;
    range = mapTxSpends.equal_range(outpoint);
    SyncMetaData(range);
}

void CWallet::RemoveFromSpends(const COutPoint& outpoint, const uint256& wtxid)
{
    std::pair<TxSpends::iterator, TxSpends::iterator> range;
    range = mapTxSpends.equal_range(outpoint);
    TxSpends::iterator it = range.first;
    for(; it != range.second; ++ it)
    {
        if(it->second == wtxid)
        {
            mapTxSpends.erase(it);
            break;
        }
    }
    range = mapTxSpends.equal_range(outpoint);
    SyncMetaData(range);
}

void CWallet::AddToSpends(const uint256& wtxid)
{
    assert(mapWallet.count(wtxid));
    CWalletTx& thisTx = mapWallet[wtxid];
    if (thisTx.IsCoinBase()) // Coinbases don't spend anything!
        return;

    for (const CTxIn& txin : thisTx.tx->vin)
        AddToSpends(txin.prevout, wtxid);
}

void CWallet::RemoveFromSpends(const uint256& wtxid)
{
    assert(mapWallet.count(wtxid));
    CWalletTx& thisTx = mapWallet[wtxid];
    if (thisTx.IsCoinBase()) // Coinbases don't spend anything!
        return;

    for(const CTxIn& txin : thisTx.tx->vin)
        RemoveFromSpends(txin.prevout, wtxid);
}

bool CWallet::GetVinAndKeysFromOutput(COutput out, CTxIn& txinRet, CPubKey& pubKeyRet, CKey& keyRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    CScript pubScript;

    txinRet = CTxIn(out.tx->GetHash(), out.i);
    pubScript = out.tx->tx->vout[out.i].scriptPubKey; // the inputs PubKey

    CTxDestination address1;
    ExtractDestination(pubScript, address1);

    const CKeyID *keyID = boost::get<CKeyID>(&address1);
    if (!keyID) {
        LogPrintf("CWallet::GetVinAndKeysFromOutput -- Address does not refer to a key\n");
        return false;
    }

    if (!GetKey(*keyID, keyRet)) {
        LogPrintf("CWallet::GetVinAndKeysFromOutput -- Private key for address is not known\n");
        return false;
    }

    pubKeyRet = keyRet.GetPubKey();
    return true;
}

bool CWallet::EncryptWallet(const SecureString& strWalletPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial _vMasterKey;

    _vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    GetStrongRandBytes(&_vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey;

    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    GetStrongRandBytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64_t nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = 2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    LogPrintf("Encrypting Wallet with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(_vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK(cs_wallet);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        assert(!pwalletdbEncryption);
        pwalletdbEncryption = new CWalletDB(strWalletFile);
        if (!pwalletdbEncryption->TxnBegin()) {
            delete pwalletdbEncryption;
            pwalletdbEncryption = nullptr;
            return false;
        }
        pwalletdbEncryption->WriteMasterKey(nMasterKeyMaxID, kMasterKey);

        if (!EncryptKeys(_vMasterKey))
        {
            pwalletdbEncryption->TxnAbort();
            delete pwalletdbEncryption;
            // We now probably have half of our keys encrypted in memory, and half not...
            // die and let the user reload their unencrypted wallet.
            assert(false);
        }

        // Encryption was introduced in version 0.4.0
        SetMinVersion(FEATURE_WALLETCRYPT, pwalletdbEncryption, true);

        if (!pwalletdbEncryption->TxnCommit()) {
            delete pwalletdbEncryption;
            // We now have keys encrypted in memory, but not on disk...
            // die to avoid confusion and let the user reload the unencrypted wallet.
            assert(false);
        }

        delete pwalletdbEncryption;
        pwalletdbEncryption = nullptr;

        Lock();
        Unlock(strWalletPassphrase);

        // if we are using HD, replace the HD master key (seed) with a new one
        if (IsHDEnabled()) {
            if (!SetHDMasterKey(GenerateNewHDMasterKey())) {
                return false;
            }
        }

        NewKeyPool();
        Lock();

        // Need to completely rewrite the wallet file; if we don't, bdb might keep
        // bits of the unencrypted private key in slack space in the database file.
        CDB::Rewrite(strWalletFile);

     }
    NotifyStatusChanged(this);

    return true;
}

DBErrors CWallet::ReorderTransactions()
{
    LOCK(cs_wallet);
    CWalletDB walletdb(strWalletFile);

    // Old wallets didn't have any defined order for transactions
    // Probably a bad idea to change the output of this

    // First: get all CWalletTx and CAccountingEntry into a sorted-by-time multimap.
    typedef std::pair<CWalletTx*, CAccountingEntry*> TxPair;
    typedef std::multimap<int64_t, TxPair > TxItems;
    TxItems txByTime;

    for (std::map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        CWalletTx* wtx = &((*it).second);
        txByTime.insert(std::make_pair(wtx->nTimeReceived, TxPair(wtx, (CAccountingEntry*)0)));
    }
    std::list<CAccountingEntry> acentries;
    walletdb.ListAccountCreditDebit("", acentries);
    for (CAccountingEntry& entry : acentries)
    {
        txByTime.insert(std::make_pair(entry.nTime, TxPair((CWalletTx*)0, &entry)));
    }

    nOrderPosNext = 0;
    std::vector<int64_t> nOrderPosOffsets;
    for (TxItems::iterator it = txByTime.begin(); it != txByTime.end(); ++it)
    {
        CWalletTx *const pwtx = (*it).second.first;
        CAccountingEntry *const pacentry = (*it).second.second;
        int64_t& nOrderPos = (pwtx != 0) ? pwtx->nOrderPos : pacentry->nOrderPos;

        if (nOrderPos == -1)
        {
            nOrderPos = nOrderPosNext++;
            nOrderPosOffsets.push_back(nOrderPos);

            if (pwtx)
            {
                if (!walletdb.WriteTx(*pwtx))
                    return DB_LOAD_FAIL;
            }
            else
            if (!walletdb.WriteAccountingEntry(pacentry->nEntryNo, *pacentry))
                return DB_LOAD_FAIL;
        }
        else
        {
            int64_t nOrderPosOff = 0;
            for (const int64_t& nOffsetStart : nOrderPosOffsets)
            {
                if (nOrderPos >= nOffsetStart)
                    ++nOrderPosOff;
            }
            nOrderPos += nOrderPosOff;
            nOrderPosNext = std::max(nOrderPosNext, nOrderPos + 1);

            if (!nOrderPosOff)
                continue;

            // Since we're changing the order, write it back
            if (pwtx)
            {
                if (!walletdb.WriteTx(*pwtx))
                    return DB_LOAD_FAIL;
            }
            else
            if (!walletdb.WriteAccountingEntry(pacentry->nEntryNo, *pacentry))
                return DB_LOAD_FAIL;
        }
    }
    walletdb.WriteOrderPosNext(nOrderPosNext);

    return DB_LOAD_OK;
}

int64_t CWallet::IncOrderPosNext(CWalletDB* pwalletdb)
{
    AssertLockHeld(cs_wallet); // nOrderPosNext
    int64_t nRet = nOrderPosNext++;
    if (pwalletdb) {
        pwalletdb->WriteOrderPosNext(nOrderPosNext);
    } else {
        CWalletDB(strWalletFile).WriteOrderPosNext(nOrderPosNext);
    }
    return nRet;
}

bool CWallet::AccountMove(std::string strFrom, std::string strTo, CAmount nAmount, std::string strComment)
{
    CWalletDB walletdb(strWalletFile);
    if (!walletdb.TxnBegin())
        return false;

    int64_t nNow = GetAdjustedTime();

    // Debit
    CAccountingEntry debit;
    debit.nOrderPos = IncOrderPosNext(&walletdb);
    debit.strAccount = strFrom;
    debit.nCreditDebit = -nAmount;
    debit.nTime = nNow;
    debit.strOtherAccount = strTo;
    debit.strComment = strComment;
    AddAccountingEntry(debit, &walletdb);

    // Credit
    CAccountingEntry credit;
    credit.nOrderPos = IncOrderPosNext(&walletdb);
    credit.strAccount = strTo;
    credit.nCreditDebit = nAmount;
    credit.nTime = nNow;
    credit.strOtherAccount = strFrom;
    credit.strComment = strComment;
    AddAccountingEntry(credit, &walletdb);

    if (!walletdb.TxnCommit())
        return false;

    return true;
}

bool CWallet::GetAccountDestination(CTxDestination &dest, std::string strAccount, bool bForceNew) {
    CWalletDB walletdb(strAccount);

    CAccount account;
    walletdb.ReadAccount(strAccount, account);

    if (!bForceNew) {
        if (!account.vchPubKey.IsValid())
            bForceNew = true;
        else {
            // Check if the current key has been used (TODO: check other addresses with the same key)
            CScript scriptPubKey = GetScriptForDestination(GetDestinationForKey(account.vchPubKey, g_address_type));
            for (std::map<uint256, CWalletTx>::iterator it = mapWallet.begin();
                 it != mapWallet.end() && account.vchPubKey.IsValid();
                 ++it)
                for (const CTxOut& txout : (*it).second.tx->vout)
                    if (txout.scriptPubKey == scriptPubKey) {
                        bForceNew = true;
                        break;
                    }
        }
    }

    // Generate a new key
    if (bForceNew) {
        if (!GetKeyFromPool(account.vchPubKey, false))
            return false;

        LearnRelatedScripts(account.vchPubKey, g_address_type);
        dest = GetDestinationForKey(account.vchPubKey, g_address_type);
        SetAddressBook(dest, strAccount, "receive");
        walletdb.WriteAccount(strAccount, account);
    } else {
        dest = GetDestinationForKey(account.vchPubKey, g_address_type);
    }

    return true;
}

CWallet::TxItems CWallet::OrderedTxItems(std::list<CAccountingEntry>& acentries, std::string strAccount)
{
    AssertLockHeld(cs_wallet); // mapWallet
    CWalletDB walletdb(strWalletFile);

    // First: get all CWalletTx and CAccountingEntry into a sorted-by-order multimap.
    TxItems txOrdered;

    // Note: maintaining indices in the database of (account,time) --> txid and (account, time) --> acentry
    // would make this much faster for applications that do this a lot.
    for (std::map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
        CWalletTx *wtx = &((*it).second);
        txOrdered.insert(std::make_pair(wtx->nOrderPos, TxPair(wtx, (CAccountingEntry *) 0)));
    }
    acentries.clear();
    walletdb.ListAccountCreditDebit(strAccount, acentries);
    for (CAccountingEntry& entry : acentries) {
        txOrdered.insert(std::make_pair(entry.nOrderPos, TxPair((CWalletTx*)0, &entry)));
    }

    return txOrdered;
}

void CWallet::MarkDirty()
{
    {
        LOCK(cs_wallet);
        for (std::pair<const uint256, CWalletTx>& item : mapWallet)
            item.second.MarkDirty();
    }
}

bool CWallet::AddToWallet(const CWalletTx& wtxIn, bool fFromLoadWallet, bool fFlushOnClose)
{
    CWalletDB walletdb(strWalletFile, "r+", fFlushOnClose);
    uint256 hash = wtxIn.GetHash();

    if (fFromLoadWallet) {
        mapWallet[hash] = wtxIn;
        mapWallet[hash].BindWallet(this);
        AddToSpends(hash);
    } else {
        LOCK(cs_wallet);
        // Inserts only if not already there, returns tx inserted or tx found
        std::pair<std::map<uint256, CWalletTx>::iterator, bool> ret = mapWallet.insert(std::make_pair(hash, wtxIn));
        CWalletTx& wtx = (*ret.first).second;
        wtx.BindWallet(this);
        bool fInsertedNew = ret.second;
        if (fInsertedNew) {
            wtx.nTimeReceived = GetAdjustedTime();
            wtx.nOrderPos = IncOrderPosNext();

            wtx.nTimeSmart = wtx.nTimeReceived;
            if (wtxIn.hashBlock != 0) {
                if (mapBlockIndex.count(wtxIn.hashBlock)) {
                    int64_t latestNow = wtx.nTimeReceived;
                    int64_t latestEntry = 0;
                    {
                        // Tolerate times up to the last timestamp in the wallet not more than 5 minutes into the future
                        int64_t latestTolerated = latestNow + 300;
                        std::list<CAccountingEntry> acentries;
                        TxItems txOrdered = OrderedTxItems(acentries);
                        for (TxItems::reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
                            CWalletTx* const pwtx = (*it).second.first;
                            if (pwtx == &wtx)
                                continue;
                            CAccountingEntry* const pacentry = (*it).second.second;
                            int64_t nSmartTime;
                            if (pwtx) {
                                nSmartTime = pwtx->nTimeSmart;
                                if (!nSmartTime)
                                    nSmartTime = pwtx->nTimeReceived;
                            } else
                                nSmartTime = pacentry->nTime;
                            if (nSmartTime <= latestTolerated) {
                                latestEntry = nSmartTime;
                                if (nSmartTime > latestNow)
                                    latestNow = nSmartTime;
                                break;
                            }
                        }
                    }

                    int64_t blocktime = mapBlockIndex[wtxIn.hashBlock]->GetBlockTime();
                    wtx.nTimeSmart = std::max(latestEntry, std::min(blocktime, latestNow));
                } else
                    LogPrintf("AddToWallet() : found %s in block %s not in index\n",
                        wtxIn.GetHash().ToString(),
                        wtxIn.hashBlock.ToString());
            }
            AddToSpends(hash);
        }

        bool fUpdated = false;
        if (!fInsertedNew) {
            // Merge
            if (wtxIn.hashBlock != 0 && wtxIn.hashBlock != wtx.hashBlock) {
                wtx.hashBlock = wtxIn.hashBlock;
                fUpdated = true;
            }
            if (wtxIn.nIndex != -1 && (wtxIn.vMerkleBranch != wtx.vMerkleBranch || wtxIn.nIndex != wtx.nIndex)) {
                wtx.vMerkleBranch = wtxIn.vMerkleBranch;
                wtx.nIndex = wtxIn.nIndex;
                fUpdated = true;
            }
            if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe) {
                wtx.fFromMe = wtxIn.fFromMe;
                fUpdated = true;
            }
            // If we have a witness-stripped version of this transaction, and we
            // see a new version with a witness, then we must be upgrading a pre-segwit
            // wallet.  Store the new version of the transaction with the witness,
            // as the stripped-version must be invalid.
            // TODO: Store all versions of the transaction, instead of just one.
            if (wtxIn.HasWitness() && !wtx.HasWitness()) {
                wtx.SetTx(wtxIn.tx);
                fUpdated = true;
            }
        }

        //// debug print
        LogPrintf("AddToWallet %s  %s%s\n", wtxIn.GetHash().ToString(), (fInsertedNew ? "new" : ""), (fUpdated ? "update" : ""));

        // Write to disk
        if (fInsertedNew || fUpdated)
            if (!wtx.WriteToDisk())
                return false;

        // Break debit/credit balance caches:
        wtx.MarkDirty();

        // Notify UI of new or updated transaction
        NotifyTransactionChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

        // notify an external script when a wallet transaction comes in or is updated
        std::string strCmd = GetArg("-walletnotify", "");

        if (!strCmd.empty()) {
            boost::replace_all(strCmd, "%s", wtxIn.GetHash().GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }
    }
    return true;
}

bool CWallet::LoadToWallet(const CWalletTx& wtxIn)
{
    uint256 hash = wtxIn.GetHash();

    mapWallet[hash] = wtxIn;
    CWalletTx& wtx = mapWallet[hash];
    wtx.BindWallet(this);
    wtxOrdered.insert(std::make_pair(wtx.nOrderPos, TxPair(&wtx, (CAccountingEntry*)0)));
    AddToSpends(hash);
    for (const CTxIn& txin : wtx.tx->vin) {
        if (mapWallet.count(txin.prevout.hash)) {
            CWalletTx& prevtx = mapWallet[txin.prevout.hash];
            if (prevtx.nIndex == -1 && !prevtx.hashUnset()) {
                MarkConflicted(prevtx.hashBlock, wtx.GetHash());
            }
        }
    }

    return true;
}

/**
 * Add a transaction to the wallet, or update it.
 * pblock is optional, but should be provided if the transaction is known to be in a block.
 * If fUpdate is true, existing transactions will be updated.
 */
bool CWallet::AddToWalletIfInvolvingMe(const CTransactionRef& ptx, const CBlockIndex* pIndex, int posInBlock, bool fUpdate)
{
    const CTransaction& tx = *ptx;
    {
        AssertLockHeld(cs_wallet);

        if (pIndex != nullptr) {
            for (const CTxIn& txin : tx.vin) {
                std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range = mapTxSpends.equal_range(txin.prevout);
                while (range.first != range.second) {
                    if (range.first->second != tx.GetHash()) {
                        LogPrintf("Transaction %s (in block %s) conflicts with wallet transaction %s (both spend %s:%i)\n", tx.GetHash().ToString(), pIndex->GetBlockHash().ToString(), range.first->second.ToString(), range.first->first.hash.ToString(), range.first->first.n);
                        MarkConflicted(pIndex->GetBlockHash(), range.first->second);
                    }
                    range.first++;
                }
            }
        }

        bool fExisted = mapWallet.count(tx.GetHash()) != 0;
        if (fExisted && !fUpdate) return false;
        if (fExisted || IsMine(tx) || IsFromMe(tx))
        {
            /* Check if any keys in the wallet keypool that were supposed to be unused
             * have appeared in a new transaction. If so, remove those keys from the keypool.
             * This can happen when restoring an old wallet backup that does not contain
             * the mostly recently created transactions from newer versions of the wallet.
             */

            // loop though all outputs
            for (const CTxOut& txout: tx.vout) {
                // extract addresses and check if they match with an unused keypool key
                std::vector<CKeyID> vAffected;
                CAffectedKeysVisitor(*this, vAffected).Process(txout.scriptPubKey);
                for (const CKeyID &keyid : vAffected) {
                    std::map<CKeyID, int64_t>::const_iterator mi = m_pool_key_to_index.find(keyid);
                    if (mi != m_pool_key_to_index.end()) {
                        LogPrintf("%s: Detected a used keypool key, mark all keypool key up to this key as used\n", __func__);
                        MarkReserveKeysAsUsed(mi->second);

                        if (!TopUpKeyPool()) {
                            LogPrintf("%s: Topping up keypool failed (locked wallet)\n", __func__);
                        }
                    }
                }
            }

            CWalletTx wtx(this, ptx);

            // Get merkle branch if transaction was found in a block
            if (pIndex != nullptr)
                wtx.SetMerkleBranch(pIndex, posInBlock);

            return AddToWallet(wtx, false);
        }
    }
    return false;
}

bool CWallet::TransactionCanBeAbandoned(const uint256& hashTx) const
{
    LOCK2(cs_main, cs_wallet);
    const CWalletTx* wtx = GetWalletTx(hashTx);
    return wtx && !wtx->isAbandoned() && wtx->GetDepthInMainChain() <= 0 && !wtx->InMempool();
}

bool CWallet::AbandonTransaction(const uint256& hashTx)
{
    LOCK2(cs_main, cs_wallet);

    CWalletDB walletdb(strWalletFile, "r+");

    std::set<uint256> todo;
    std::set<uint256> done;

    // Can't mark abandoned if confirmed or in mempool
    assert(mapWallet.count(hashTx));
    CWalletTx& origtx = mapWallet[hashTx];
    if (origtx.GetDepthInMainChain() > 0 || origtx.InMempool()) {
        return false;
    }

    todo.insert(hashTx);

    while (!todo.empty()) {
        uint256 now = *todo.begin();
        todo.erase(now);
        done.insert(now);
        assert(mapWallet.count(now));
        CWalletTx& wtx = mapWallet[now];
        int currentconfirm = wtx.GetDepthInMainChain();
        // If the orig tx was not in block, none of its spends can be
        assert(currentconfirm <= 0);
        // if (currentconfirm < 0) {Tx and spends are already conflicted, no need to abandon}
        if (currentconfirm == 0 && !wtx.isAbandoned()) {
            // If the orig tx was not in block/mempool, none of its spends can be in mempool
            assert(!wtx.InMempool());
            wtx.nIndex = -1;
            wtx.setAbandoned();
            wtx.MarkDirty();
            walletdb.WriteTx(wtx);
            NotifyTransactionChanged(this, wtx.GetHash(), CT_UPDATED);
            // Iterate over all its outputs, and mark transactions in the wallet that spend them abandoned too
            TxSpends::const_iterator iter = mapTxSpends.lower_bound(COutPoint(hashTx, 0));
            while (iter != mapTxSpends.end() && iter->first.hash == now) {
                if (!done.count(iter->second)) {
                    todo.insert(iter->second);
                }
                iter++;
            }
            // If a transaction changes 'conflicted' state, that changes the balance
            // available of the outputs it spends. So force those to be recomputed
            for (const CTxIn& txin : wtx.tx->vin)
            {
                if (mapWallet.count(txin.prevout.hash))
                    mapWallet[txin.prevout.hash].MarkDirty();
            }
        }
    }

    return true;
}

void CWallet::MarkConflicted(const uint256& hashBlock, const uint256& hashTx)
{
    LOCK2(cs_main, cs_wallet);

    int conflictconfirms = 0;
    if (mapBlockIndex.count(hashBlock)) {
        CBlockIndex* pindex = mapBlockIndex[hashBlock];
        if (chainActive.Contains(pindex)) {
            conflictconfirms = -(chainActive.Height() - pindex->nHeight + 1);
        }
    }
    // If number of conflict confirms cannot be determined, this means
    // that the block is still unknown or not yet part of the main chain,
    // for example when loading the wallet during a reindex. Do nothing in that
    // case.
    if (conflictconfirms >= 0)
        return;

    // Do not flush the wallet here for performance reasons
    CWalletDB walletdb(strWalletFile, "r+", false);

    std::set<uint256> todo;
    std::set<uint256> done;

    todo.insert(hashTx);

    while (!todo.empty()) {
        uint256 now = *todo.begin();
        todo.erase(now);
        done.insert(now);
        assert(mapWallet.count(now));
        CWalletTx& wtx = mapWallet[now];
        int currentconfirm = wtx.GetDepthInMainChain();
        if (conflictconfirms < currentconfirm) {
            // Block is 'more conflicted' than current confirm; update.
            // Mark transaction as conflicted with this block.
            wtx.nIndex = -1;
            wtx.hashBlock = hashBlock;
            wtx.MarkDirty();
            walletdb.WriteTx(wtx);
            // Iterate over all its outputs, and mark transactions in the wallet that spend them conflicted too
            TxSpends::const_iterator iter = mapTxSpends.lower_bound(COutPoint(now, 0));
            while (iter != mapTxSpends.end() && iter->first.hash == now) {
                if (!done.count(iter->second)) {
                    todo.insert(iter->second);
                }
                iter++;
            }
            // If a transaction changes 'conflicted' state, that changes the balance
            // available of the outputs it spends. So force those to be recomputed
            for (const CTxIn& txin : wtx.tx->vin)
            {
                if (mapWallet.count(txin.prevout.hash))
                    mapWallet[txin.prevout.hash].MarkDirty();
            }
        }
    }
}

void CWallet::SyncTransaction(const CTransactionRef& ptx, const CBlockIndex *pindex, int posInBlock) {
    const CTransaction& tx = *ptx;

    if (!pindex && posInBlock == -1)
    {
        // wallets need to refund inputs when disconnecting coinstake
        if (tx.IsCoinStake() && IsFromMe(tx))
        {
            DisableTransaction(tx);
            return;
        }
    }

    if (!AddToWalletIfInvolvingMe(ptx, pindex, posInBlock, true))
        return; // Not one of ours

    // If a transaction changes 'conflicted' state, that changes the balance
    // available of the outputs it spends. So force those to be
    // recomputed, also:
    for (const CTxIn& txin : tx.vin)
    {
        if (mapWallet.count(txin.prevout.hash))
            mapWallet[txin.prevout.hash].MarkDirty();
    }
}

void CWallet::EraseFromWallet(const uint256& hash)
{
    if (!fFileBacked)
        return;
    {
        LOCK(cs_wallet);
        if (mapWallet.erase(hash))
            CWalletDB(strWalletFile).EraseTx(hash);
    }
    return;
}


isminetype CWallet::IsMine(const CTxIn& txin) const
{
    {
        LOCK(cs_wallet);
        std::map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end()) {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.tx->vout.size())
                return IsMine(prev.tx->vout[txin.prevout.n]);
        }
    }
    return ISMINE_NO;
}

CAmount CWallet::GetDebit(const CTxIn& txin, const isminefilter& filter) const
{
    {
        LOCK(cs_wallet);
        std::map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end()) {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.tx->vout.size())
                if (IsMine(prev.tx->vout[txin.prevout.n]) & filter)
                    return prev.tx->vout[txin.prevout.n].nValue;
        }
    }
    return 0;
}

// Recursively determine the rounds of a given input (How deep is the DarkSend chain for a given input)
int CWallet::GetRealInputDarkSendRounds(CTxIn in, int rounds) const
{
    static std::map<uint256, CMutableTransaction> mDenomWtxes;

    if (rounds >= 16) return 15; // 16 rounds max

    uint256 hash = in.prevout.hash;
    unsigned int nout = in.prevout.n;

    const CWalletTx* wtx = GetWalletTx(hash);
    if (wtx != nullptr) {
        std::map<uint256, CMutableTransaction>::const_iterator mdwi = mDenomWtxes.find(hash);
        // not known yet, let's add it
        if (mdwi == mDenomWtxes.end()) {
            LogPrint(BCLog::DARKSEND, "GetInputDarkSendRounds INSERTING %s\n", hash.ToString());
            mDenomWtxes[hash] = CMutableTransaction(*wtx->tx);
        }
        // found and it's not an initial value, just return it
        else if (mDenomWtxes[hash].vout[nout].nRounds != -10) {
            return mDenomWtxes[hash].vout[nout].nRounds;
        }


        // bounds check
        if (nout >= wtx->tx->vout.size()) {
            // should never actually hit this
            LogPrint(BCLog::DARKSEND, "GetInputDarkSendRounds UPDATED   %s %3d %3d\n", hash.ToString(), nout, -4);
            return -4;
        }

        if (pwalletMain->IsCollateralAmount(wtx->tx->vout[nout].nValue)) {
            mDenomWtxes[hash].vout[nout].nRounds = -3;
            LogPrint(BCLog::DARKSEND, "GetInputDarkSendRounds UPDATED   %s %3d %3d\n", hash.ToString(), nout, mDenomWtxes[hash].vout[nout].nRounds);
            return mDenomWtxes[hash].vout[nout].nRounds;
        }

        //make sure the final output is non-denominate
        if (/*rounds == 0 && */ !IsDenominatedAmount(wtx->tx->vout[nout].nValue)) //NOT DENOM
        {
            mDenomWtxes[hash].vout[nout].nRounds = -2;
            LogPrint(BCLog::DARKSEND, "GetInputDarkSendRounds UPDATED   %s %3d %3d\n", hash.ToString(), nout, mDenomWtxes[hash].vout[nout].nRounds);
            return mDenomWtxes[hash].vout[nout].nRounds;
        }

        bool fAllDenoms = true;
        for (CTxOut out : wtx->tx->vout) {
            fAllDenoms = fAllDenoms && IsDenominatedAmount(out.nValue);
        }
        // this one is denominated but there is another non-denominated output found in the same tx
        if (!fAllDenoms) {
            mDenomWtxes[hash].vout[nout].nRounds = 0;
            LogPrint(BCLog::DARKSEND, "GetInputDarkSendRounds UPDATED   %s %3d %3d\n", hash.ToString(), nout, mDenomWtxes[hash].vout[nout].nRounds);
            return mDenomWtxes[hash].vout[nout].nRounds;
        }

        int nShortest = -10; // an initial value, should be no way to get this by calculations
        bool fDenomFound = false;
        // only denoms here so let's look up
        for (CTxIn in2 : wtx->tx->vin) {
            if (IsMine(in2)) {
                int n = GetRealInputDarkSendRounds(in2, rounds + 1);
                // denom found, find the shortest chain or initially assign nShortest with the first found value
                if (n >= 0 && (n < nShortest || nShortest == -10)) {
                    nShortest = n;
                    fDenomFound = true;
                }
            }
        }
        mDenomWtxes[hash].vout[nout].nRounds = fDenomFound ? (nShortest >= 15 ? 16 : nShortest + 1) // good, we a +1 to the shortest one but only 16 rounds max allowed
                                                             :
                                                             0; // too bad, we are the fist one in that chain
        LogPrint(BCLog::DARKSEND, "GetInputDarkSendRounds UPDATED   %s %3d %3d\n", hash.ToString(), nout, mDenomWtxes[hash].vout[nout].nRounds);
        return mDenomWtxes[hash].vout[nout].nRounds;
    }

    return rounds - 1;
}

// respect current settings
int CWallet::GetInputDarkSendRounds(CTxIn in) const
{
    LOCK(cs_wallet);
    int realDarkSendRounds = GetRealInputDarkSendRounds(in, 0);
    return realDarkSendRounds > nDarksendRounds ? nDarksendRounds : realDarkSendRounds;
}

bool CWallet::IsDenominated(const CTxIn& txin) const
{
    {
        LOCK(cs_wallet);
        std::map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end()) {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.tx->vout.size()) return IsDenominatedAmount(prev.tx->vout[txin.prevout.n].nValue);
        }
    }
    return false;
}

bool CWallet::IsDenominated(const CTransaction& tx) const
{
    /*
        Return false if ANY inputs are non-denom
    */
    bool ret = true;
    for (const CTxIn& txin : tx.vin) {
        if (!IsDenominated(txin)) {
            ret = false;
        }
    }
    return ret;
}


bool CWallet::IsDenominatedAmount(int64_t nInputAmount) const
{
    for (int64_t d : darkSendDenominations)
        if (nInputAmount == d)
            return true;
    return false;
}

bool CWallet::IsChange(const CTxOut& txout) const
{
    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a script that is ours, but is not in the address book
    // is change. That assumption is likely to break when we implement multisignature
    // wallets that return change back into a multi-signature-protected address;
    // a better way of identifying which outputs are 'the send' and which are
    // 'the change' will need to be implemented (maybe extend CWalletTx to remember
    // which output, if any, was change).
    if (::IsMine(*this, txout.scriptPubKey)) {
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address))
            return true;

        LOCK(cs_wallet);
        if (!mapAddressBook.count(address))
            return true;
    }
    return false;
}

CPubKey CWallet::GenerateNewHDMasterKey()
{
    CKey key;
    key.MakeNewKey(true);

    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    // calculate the pubkey
    CPubKey pubkey = key.GetPubKey();
    assert(key.VerifyPubKey(pubkey));

    // set the hd keypath to "m" -> Master, refers the masterkeyid to itself
    metadata.hdKeypath     = "m";
    metadata.hdMasterKeyID = pubkey.GetID();

    {
        LOCK(cs_wallet);

        // mem store the metadata
        mapKeyMetadata[pubkey.GetID()] = metadata;

        // write the key&metadata to the database
        if (!AddKeyPubKey(key, pubkey))
            throw std::runtime_error(std::string(__func__) + ": AddKeyPubKey failed");
    }

    return pubkey;
}

bool CWallet::SetHDMasterKey(const CPubKey& pubkey)
{
    LOCK(cs_wallet);
    // store the keyid (hash160) together with
    // the child index counter in the database
    // as a hdchain object
    CHDChain newHdChain;
    newHdChain.nVersion = CanSupportFeature(FEATURE_HD_SPLIT) ? CHDChain::VERSION_HD_CHAIN_SPLIT : CHDChain::VERSION_HD_BASE;
    newHdChain.masterKeyID = pubkey.GetID();
    SetHDChain(newHdChain, false);

    return true;
}

bool CWallet::SetHDChain(const CHDChain& chain, bool memonly)
{
    LOCK(cs_wallet);
    if (!memonly && !CWalletDB(strWalletFile).WriteHDChain(chain))
        throw std::runtime_error(std::string(__func__) + ": writing chain failed");

    hdChain = chain;
    return true;
}

bool CWallet::IsHDEnabled() const
{
    return !hdChain.masterKeyID.IsNull();
}

void CWallet::SetWalletFlag(uint64_t flags)
{
    LOCK(cs_wallet);
    m_wallet_flags |= flags;
    if (!CWalletDB(strWalletFile).WriteWalletFlags(m_wallet_flags))
        throw std::runtime_error(std::string(__func__) + ": writing wallet flags failed");
}

bool CWallet::IsWalletFlagSet(uint64_t flag)
{
    return (m_wallet_flags & flag);
}

bool CWallet::SetWalletFlags(uint64_t overwriteFlags, bool memonly)
{
    LOCK(cs_wallet);
    m_wallet_flags = overwriteFlags;
    if (((overwriteFlags & g_known_wallet_flags) >> 32) ^ (overwriteFlags >> 32)) {
        // contains unknown non-tolerable wallet flags
        return false;
    }
    if (!memonly && !CWalletDB(strWalletFile).WriteWalletFlags(m_wallet_flags)) {
        throw std::runtime_error(std::string(__func__) + ": writing wallet flags failed");
    }

    return true;
}

int64_t CWalletTx::GetTxTime() const
{
    int64_t n = nTimeSmart;
    return n ? n : nTimeReceived;
}

int CWalletTx::GetRequestCount() const
{
    // Returns -1 if it wasn't being tracked
    int nRequests = -1;
    {
        LOCK(pwallet->cs_wallet);
        if (IsCoinBase()) {
            // Generated block
            if (hashBlock != 0) {
                std::map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                if (mi != pwallet->mapRequestCount.end())
                    nRequests = (*mi).second;
            }
        } else {
            // Did anyone request this transaction?
            std::map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(GetHash());
            if (mi != pwallet->mapRequestCount.end()) {
                nRequests = (*mi).second;

                // How about the block it's in?
                if (nRequests == 0 && hashBlock != 0) {
                    std::map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                    if (mi != pwallet->mapRequestCount.end())
                        nRequests = (*mi).second;
                    else
                        nRequests = 1; // If it's in someone else's block it must have got out
                }
            }
        }
    }
    return nRequests;
}

void CWalletTx::GetAmounts(std::list<COutputEntry>& listReceived, std::list<COutputEntry>& listSent, CAmount& nFee, std::string& strSentAccount, const isminefilter& filter) const
{
    nFee = 0;
    listReceived.clear();
    listSent.clear();
    strSentAccount = strFromAccount;

    // Compute fee:
    CAmount nDebit = GetDebit(filter);
    if (nDebit > 0) // debit>0 means we signed/sent this transaction
    {
        CAmount nValueOut = GetValueOut();
        nFee = nDebit - nValueOut;
    }

    // Sent/received.
    for (unsigned int i = 0; i < tx->vout.size(); ++i) {
        const CTxOut& txout = tx->vout[i];
        isminetype fIsMine = pwallet->IsMine(txout);
        // Only need to handle txouts if AT LEAST one of these is true:
        //   1) they debit from us (sent)
        //   2) the output is to us (received)
        if (nDebit > 0) {
            // Don't report 'change' txouts
            if (pwallet->IsChange(txout))
                continue;
        } else if (!(fIsMine & filter))
            continue;

        // In either case, we need to get the destination address
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address) && !txout.scriptPubKey.IsUnspendable()) {
            //LogPrintf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",
                //this->GetHash().ToString());
            address = CNoDestination();
        }

        COutputEntry output = {address, txout.nValue, (int)i};

        // If we are debited by the transaction, add the output as a "sent" entry
        if (nDebit > 0)
            listSent.push_back(output);

        // If we are receiving the output, add it as a "received" entry
        if (fIsMine & filter)
            listReceived.push_back(output);
    }
}

void CWalletTx::GetAccountAmounts(const std::string& strAccount, CAmount& nReceived, CAmount& nSent, CAmount& nFee, const isminefilter& filter) const
{
    nReceived = nSent = nFee = 0;

    CAmount allFee;
    string strSentAccount;
    list<COutputEntry> listReceived;
    list<COutputEntry> listSent;
    GetAmounts(listReceived, listSent, allFee, strSentAccount, filter);

    if (strAccount == strSentAccount) {
        for (const COutputEntry& s : listSent)
            nSent += s.amount;
        nFee = allFee;
    }
    {
        LOCK(pwallet->cs_wallet);
        for (const COutputEntry& r : listReceived) {
            if (pwallet->mapAddressBook.count(r.destination)) {
                map<CTxDestination, CAddressBookData>::const_iterator mi = pwallet->mapAddressBook.find(r.destination);
                if (mi != pwallet->mapAddressBook.end() && (*mi).second.name == strAccount)
                    nReceived += r.amount;
            } else if (strAccount.empty()) {
                nReceived += r.amount;
            }
        }
    }
}


bool CWalletTx::WriteToDisk()
{
    return CWalletDB(pwallet->strWalletFile).WriteTx(*this);
}


/**
 * Scan the block chain (starting in pindexStart) for transactions
 * from or to us. If fUpdate is true, found transactions that already
 * exist in the wallet will be updated.
 */
CBlockIndex* CWallet::ScanForWalletTransactions(CBlockIndex* pindexStart, bool fUpdate)
{
    int64_t nNow = GetTime();

    CBlockIndex* pindex = pindexStart;
    CBlockIndex* ret = nullptr;
    {
        //LOCK2(cs_main, cs_wallet);

        // no need to read and scan block, if block was created before
        // our wallet birthday (as adjusted for block time variability)
        while (pindex && nTimeFirstKey && (pindex->GetBlockTime() < (nTimeFirstKey - 7200)))
            pindex = chainActive.Next(pindex);

        fAbortRescan = false;
        ShowProgress(_("Rescanning..."), 0); // show rescan progress in GUI as dialog or on splashscreen, if -rescan on startup

        int dProgressTip;
        {
            LOCK(cs_main);
            dProgressTip = chainActive.Height();
        }

        int dProgressStart = pindex ? pindex->nHeight : 0;
        int dProgressCurrent = dProgressStart;
        int dProgressTotal = dProgressTip - dProgressStart;
        int dProgressShow = 0;
        int dProgressShowPrev = 0;

        while (pindex && !fAbortRescan && !ShutdownRequested()) {
            dProgressShow = std::min(99, (int) (((dProgressCurrent - dProgressStart) * 100) / dProgressTotal));
            dProgressShow = std::max(1, dProgressShow);

            if ((pindex->nHeight % 100 == 0) && (dProgressTotal > 0))
            {
                if (dProgressShowPrev != dProgressShow)
                {
                    dProgressShowPrev = dProgressShow;
                    ShowProgress(_("Rescanning..."), dProgressShow);
                }
            }

            if (GetTime() >= nNow + 60) {
                nNow = GetTime();
                LogPrintf("Still rescanning. At block %d. Progress=%f\n", pindex->nHeight, dProgressShow);
            }

            CBlock block;
            ReadBlockFromDisk(block, pindex, Params().GetConsensus());
            {
                LOCK(cs_wallet);
                for (size_t posInBlock = 0; posInBlock < block.vtx.size(); ++posInBlock) {
                    AddToWalletIfInvolvingMe(block.vtx[posInBlock], pindex, posInBlock, fUpdate);
                }
            }
            pindex = chainActive.Next(pindex);

            // Update current height
            if (pindex) dProgressCurrent = pindex->nHeight;
        }

        if (pindex && fAbortRescan) {
            LogPrintf("Rescan aborted at block %d. Progress=%f\n", pindex->nHeight, dProgressCurrent);
        } else if (pindex && ShutdownRequested()) {
            LogPrintf("Rescan interrupted by shutdown request at block %d. Progress=%f\n", pindex->nHeight, dProgressCurrent);
        }

        ShowProgress(_("Rescanning..."), 100); // hide progress dialog in GUI
    }
    return ret;
}

void CWallet::ReacceptWalletTransactions()
{
    if (!fBroadcastTransactions)
        return;
    LOCK2(cs_main, cs_wallet);
    std::map<int64_t, CWalletTx*> mapSorted;

    for (PAIRTYPE(const uint256, CWalletTx) & item : mapWallet)
    {
        const uint256 &wtxid = item.first;
        CWalletTx &wtx = item.second;
        assert(wtx.GetHash() == wtxid);

        int nDepth = wtx.GetDepthInMainChain();

        if (!wtx.IsCoinGenerated() && nDepth < 0) {
            mapSorted.insert(std::make_pair(wtx.nOrderPos, &wtx));
        }
    }

    // Try to add wallet transactions back to memory pool
    for (PAIRTYPE(const int64_t, CWalletTx*)& item : mapSorted)
    {
        CWalletTx& wtx = *(item.second);

            // Try to add to memory pool
            LOCK(mempool.cs);
            wtx.AcceptToMemoryPool(false);
        }
    }

bool CWalletTx::InMempool() const
{
    LOCK(mempool.cs);
    if (mempool.exists(GetHash())) {
        return true;
    }
    return false;
}

void CWalletTx::RelayWalletTransaction(std::string strCommand)
{
    assert(pwallet->GetBroadcastTransactions());
    if (!IsCoinBase() && !isAbandoned() && GetDepthInMainChain() == 0)
    {
        /* GetDepthInMainChain already catches known conflicts. */
        if (InMempool() || AcceptToMemoryPool(false)) {
            uint256 hash = GetHash();
            LogPrintf("Relaying wtx %s %s\n", GetHash().ToString(), strCommand.c_str());

            if (strCommand == "ix") {
                mapTxLockReq.insert(std::make_pair(hash, *tx));
                CreateNewLock(*tx);
                RelayTransactionLockReq(*tx, true);
            } else {
                RelayTransaction(*tx);
            }
        }
    }
}

set<uint256> CWalletTx::GetConflicts() const
{
    set<uint256> result;
    if (pwallet != nullptr) {
        uint256 myHash = GetHash();
        result = pwallet->GetConflicts(myHash);
        result.erase(myHash);
    }
    return result;
}

bool CWalletTx::IsEquivalentTo(const CWalletTx& _tx)
{
    CMutableTransaction tx1 = *this->tx;
    CMutableTransaction tx2 = *_tx.tx;
    for (auto& txin : tx1.vin) txin.scriptSig = CScript();
    for (auto& txin : tx2.vin) txin.scriptSig = CScript();
    return CTransaction(tx1) == CTransaction(tx2);
}

void CWallet::ResendWalletTransactions()
{
    // Do this infrequently and randomly to avoid giving away
    // that these are our transactions.
    if (GetTime() < nNextResend || !fBroadcastTransactions)
        return;
    bool fFirst = (nNextResend == 0);
    nNextResend = GetTime() + GetRand(30 * 60);
    if (fFirst)
        return;

    // Only do it if there's been a new block since last time
    if (nTimeBestReceived < nLastResend)
        return;
    nLastResend = GetTime();

    // Rebroadcast any of our txes that aren't in a block yet
    LogPrintf("ResendWalletTransactions()\n");
    {
        LOCK(cs_wallet);
        // Sort them in chronological order
        multimap<unsigned int, CWalletTx*> mapSorted;
        for (PAIRTYPE(const uint256, CWalletTx) & item : mapWallet) {
            CWalletTx& wtx = item.second;
            // Don't rebroadcast until it's had plenty of time that
            // it should have gotten in already by now.
            if (nTimeBestReceived - (int64_t)wtx.nTimeReceived > 5 * 60)
                mapSorted.insert(std::make_pair(wtx.nTimeReceived, &wtx));
        }
        for (PAIRTYPE(const unsigned int, CWalletTx*) & item : mapSorted) {
            CWalletTx& wtx = *item.second;
            wtx.RelayWalletTransaction();
        }
    }
}

/** @} */ // end of mapWallet


/** @defgroup Actions
 *
 * @{
 */


CAmount CWallet::GetBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetAnonymizableBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAnonymizableCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetAnonymizedBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK(cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsTrusted())
            {
                int nDepth = pcoin->GetDepthInMainChain();

                for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
                    //isminetype mine = IsMine(pcoin->vout[i]);
                    bool mine = IsMine(pcoin->tx->vout[i]);
                    //COutput out = COutput(pcoin, i, nDepth, (mine & ISMINE_SPENDABLE) != ISMINE_NO);
                    COutput out = COutput(pcoin, i, nDepth, mine);
                    CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

                    if(IsSpent(out.tx->GetHash(), i) || !IsMine(pcoin->tx->vout[i]) || !IsDenominated(vin)) continue;
//                    if(pcoin->IsSpent(i) || !IsMine(pcoin->vout[i]) || !IsDenominated(vin)) continue;

                    int rounds = GetInputDarksendRounds(vin);
                    if(rounds >= nDarksendRounds){
                        nTotal += pcoin->tx->vout[i].nValue;
                    }
                }
            }
        }
    }

    return nTotal;
}


// Note: calculated including unconfirmed,
// that's ok as long as we use it for informational purposes only
double CWallet::GetAverageAnonymizedRounds() const
{
    double fTotal = 0;
    double fCount = 0;

    {
        LOCK(cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsTrusted())
            {
                int nDepth = pcoin->GetDepthInMainChain();

                for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
                    //isminetype mine = IsMine(pcoin->vout[i]);
                    bool mine = IsMine(pcoin->tx->vout[i]);
                    //COutput out = COutput(pcoin, i, nDepth, (mine & ISMINE_SPENDABLE) != ISMINE_NO);
                    COutput out = COutput(pcoin, i, nDepth, mine);
                    CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

                    if(IsSpent(out.tx->GetHash(), i) || !IsMine(pcoin->tx->vout[i]) || !IsDenominated(vin)) continue;
//                    if(pcoin->IsSpent(i) || !IsMine(pcoin->vout[i]) || !IsDenominated(vin)) continue;

                    int rounds = GetInputDarksendRounds(vin);
                    fTotal += (float)rounds;
                    fCount += 1;
                }
            }
        }
    }

    if(fCount == 0) return 0;

    return fTotal/fCount;
}


// Note: calculated including unconfirmed,
// that's ok as long as we use it for informational purposes only
CAmount CWallet::GetNormalizedAnonymizedBalance() const
{
    CAmount nTotal = 0;

    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;

            uint256 hash = (*it).first;

            for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
                CTxIn vin = CTxIn(hash, i);

                if (IsSpent(hash, i) || IsMine(pcoin->tx->vout[i]) != ISMINE_SPENDABLE || !IsDenominated(vin)) continue;
                if (pcoin->GetDepthInMainChain() < 0) continue;

                int rounds = GetInputDarkSendRounds(vin);
                nTotal += pcoin->tx->vout[i].nValue * rounds / nDarksendRounds;
            }
        }
    }

    return nTotal;
}

CAmount CWallet::GetDenominatedBalance(bool unconfirmed) const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;

            nTotal += pcoin->GetDenominatedCredit(unconfirmed);
        }
    }

    return nTotal;
}

CAmount CWallet::GetDenominatedBalance(bool onlyDenom, bool onlyUnconfirmed) const
{
    int64_t nTotal = 0;
    {
        LOCK(cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;

            int nDepth = pcoin->GetDepthInMainChain();

            // skip conflicted
            if(nDepth < 0) continue;

            bool unconfirmed = (!IsFinalTx(*pcoin->tx) || (!pcoin->IsTrusted() && nDepth == 0));
            if(onlyUnconfirmed != unconfirmed) continue;

            for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
                //isminetype mine = IsMine(pcoin->vout[i]);
                bool mine = IsMine(pcoin->tx->vout[i]);
                //COutput out = COutput(pcoin, i, nDepth, (mine & ISMINE_SPENDABLE) != ISMINE_NO);
                COutput out = COutput(pcoin, i, nDepth, mine);

                if(IsSpent(out.tx->GetHash(), i)) continue;
//                if(pcoin->IsSpent(i)) continue;
                if(!IsMine(pcoin->tx->vout[i])) continue;
                if(onlyDenom != IsDenominatedAmount(pcoin->tx->vout[i].nValue)) continue;

                nTotal += pcoin->tx->vout[i].nValue;
            }
        }
    }

    return nTotal;
}

CAmount CWallet::GetUnconfirmedBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            if (!IsFinalTx(*pcoin->tx) || (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0))
                nTotal += pcoin->GetAvailableCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetImmatureBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            nTotal += pcoin->GetImmatureCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetWatchOnlyBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted())
                nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetUnconfirmedWatchOnlyBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            if (!IsFinalTx(*pcoin->tx) || (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0))
                nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }
    return nTotal;
}

CAmount CWallet::GetImmatureWatchOnlyBalance() const
{
    CAmount nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            nTotal += pcoin->GetImmatureWatchOnlyCredit();
        }
    }
    return nTotal;
}

// Calculate total balance in a different way from GetBalance. The biggest
// difference is that GetBalance sums up all unspent TxOuts paying to the
// wallet, while this sums up both spent and unspent TxOuts paying to the
// wallet, and then subtracts the values of TxIns spending from the wallet. This
// also has fewer restrictions on which unconfirmed transactions are considered
// trusted.
CAmount CWallet::GetLegacyBalance(const isminefilter& filter, int minDepth, const std::string* account) const
{
    LOCK2(cs_main, cs_wallet);

    CAmount balance = 0;
    for (const auto& entry : mapWallet) {
        const CWalletTx& wtx = entry.second;
        const int depth = wtx.GetDepthInMainChain();
        if (depth < 0 || !CheckFinalTx(*wtx.tx) || wtx.GetBlocksToMaturity() > 0) {
            continue;
        }

        // Loop through tx outputs and add incoming payments. For outgoing txs,
        // treat change outputs specially, as part of the amount debited.
        CAmount debit = wtx.GetDebit(filter);
        const bool outgoing = debit > 0;
        for (const CTxOut& out : wtx.tx->vout) {
            if (outgoing && IsChange(out)) {
                debit -= out.nValue;
            } else if (IsMine(out) & filter && depth >= minDepth && (!account || *account == GetAccountName(out.scriptPubKey))) {
                balance += out.nValue;
            }
        }

        // For outgoing txs, subtract amount debited.
        if (outgoing && (!account || *account == wtx.strFromAccount)) {
            balance -= debit;
        }
    }

    if (account) {
        balance += CWalletDB(strWalletFile).GetAccountCreditDebit(*account);
    }

    return balance;
}

CAmount CWallet::GetAvailableBalance(const CCoinControl* coinControl) const
{
    LOCK2(cs_main, cs_wallet);

    CAmount balance = 0;
    std::vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl);
    for (const COutput& out : vCoins) {
        if (out.fSpendable) {
            balance += out.tx->tx->vout[out.i].nValue;
        }
    }
    return balance;
}

void CWallet::AvailableCoins(std::vector<COutput>& vCoins, bool fOnlyConfirmed, const CCoinControl* coinControl, bool fIncludeZeroValue, AvailableCoinsType nCoinType, bool fUseIX) const
{
    vCoins.clear();
    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const uint256& wtxid = it->first;
            const CWalletTx* pcoin = &(*it).second;

            if (!CheckFinalTx(*pcoin->tx))
                continue;

            if (fOnlyConfirmed && !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinGenerated() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            const int nDepth = pcoin->GetDepthInMainChain(false);
            // do not use IX for inputs that have less then 6 blockchain confirmations
            if (fUseIX && nDepth < 6)
                continue;

            // We should not consider coins which aren't at least in our mempool
            // It's possible for these to be conflicted via ancestors which we may never be able to detect
            if (nDepth == 0 && !pcoin->InMempool())
                continue;

            for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
                bool found = false;
                if (nCoinType == ONLY_DENOMINATED) {
                    //should make this a vector
                    found = IsDenominatedAmount(pcoin->tx->vout[i].nValue);
                } else if (nCoinType == ONLY_NONDENOMINATED || nCoinType == ONLY_NONDENOMINATED_NOTMN) {
                    // Do not use collateral or denominated amounts.
                    found = !IsCollateralAmount(pcoin->tx->vout[i].nValue);
                    if (found) found = !IsDenominatedAmount(pcoin->tx->vout[i].nValue);
                    if (found && nCoinType == ONLY_NONDENOMINATED_NOTMN) // do not use Hot MN funds
                        found = (pcoin->tx->vout[i].nValue != GetMNCollateral(chainActive.Height()) * COIN);
                } else {
                    found = true;
                }
                if (!found) continue;

                isminetype mine = IsMine(pcoin->tx->vout[i]);
                if (mine && !(IsSpent(wtxid, i)) && mine != ISMINE_NO &&
                    !IsLockedCoin((*it).first, i) && pcoin->tx->vout[i].nValue > 0 &&
                    (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i))) {
                    COutput output(pcoin, i, nDepth, mine);
#                   if defined(DEBUG_DUMP_STAKING_INFO)&&defined(DEBUG_DUMP_AvailableCoins_Coin)
                    DEBUG_DUMP_AvailableCoins_Coin();
#                   endif
                    vCoins.push_back(output);
                }
            }
        }
    }
}

void CWallet::AvailableCoinsMN(vector<COutput>& vCoins, bool fOnlyConfirmed, const CCoinControl *coinControl, AvailableCoinsType coin_type, bool useIX) const
{
    vCoins.clear();

    {
        LOCK2(cs_main, cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const uint256& wtxid = it->first;
            const CWalletTx* pcoin = &(*it).second;

            if (!IsFinalTx(*pcoin->tx))
                continue;

            if (fOnlyConfirmed && !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            if (pcoin->IsCoinStake() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            const int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth <= 0) // LuxNOTE: coincontrol fix / ignore 0 confirm
                continue;

            // do not use IX for inputs that have less then 6 blockchain confirmations
            if (useIX && nDepth < 6)
                continue;

            for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
                bool found = false;
                if(coin_type == ONLY_DENOMINATED) {
                    //should make this a vector
                    found = IsDenominatedAmount(pcoin->tx->vout[i].nValue);
                } else if(coin_type == ONLY_NONDENOMINATED || coin_type == ONLY_NONDENOMINATED_NOTMN) {
                    found = true;
                    if (IsCollateralAmount(pcoin->tx->vout[i].nValue)) continue; // do not use collateral amounts
                    found = !IsDenominatedAmount(pcoin->tx->vout[i].nValue);
                    if(found && coin_type == ONLY_NONDENOMINATED_NOTMN) found = (pcoin->tx->vout[i].nValue != GetMNCollateral(chainActive.Height()) * COIN); // do not use MN funds
                } else {
                    found = true;
                }
                if(!found) continue;

                bool mine = IsMine(pcoin->tx->vout[i]);

                if (!(IsSpent(wtxid, i)) &&
                    !IsLockedCoin((*it).first, i) && pcoin->tx->vout[i].nValue > 0 &&
                    (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
                    vCoins.push_back(COutput(pcoin, i, nDepth, mine));
            }
        }
    }
}

map<CTxDestination, vector<COutput> > CWallet::AvailableCoinsByAddress(bool fConfirmed, CAmount maxCoinValue)
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins, fConfirmed);

    map<CTxDestination, vector<COutput> > mapCoins;
    for (COutput out : vCoins) {
        if (maxCoinValue > 0 && out.tx->tx->vout[out.i].nValue > maxCoinValue)
            continue;

        CTxDestination address;
        if (!ExtractDestination(out.tx->tx->vout[out.i].scriptPubKey, address))
            continue;

        mapCoins[address].push_back(out);
    }

    return mapCoins;
}

static void ApproximateBestSubset(vector<pair<CAmount, pair<const CWalletTx*, unsigned int> > > vValue, const CAmount& nTotalLower, const CAmount& nTargetValue, vector<char>& vfBest, CAmount& nBest, int iterations = 1000)
{
    vector<char> vfIncluded;

    vfBest.assign(vValue.size(), true);
    nBest = nTotalLower;

    seed_insecure_rand();

    for (int nRep = 0; nRep < iterations && nBest != nTargetValue; nRep++) {
        vfIncluded.assign(vValue.size(), false);
        CAmount nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++) {
            for (unsigned int i = 0; i < vValue.size(); i++) {
                //The solver here uses a randomized algorithm,
                //the randomness serves no real security purpose but is just
                //needed to prevent degenerate behavior and it is important
                //that the rng is fast. We do not use a constant random sequence,
                //because there may be some privacy improvement by making
                //the selection random.
                if (nPass == 0 ? insecure_rand() & 1 : !vfIncluded[i]) {
                    nTotal += vValue[i].first;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue) {
                        fReachedTarget = true;
                        if (nTotal < nBest) {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }
                        nTotal -= vValue[i].first;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }
}


// TODO: find appropriate place for this sort function
// move denoms down
bool less_then_denom(const COutput& out1, const COutput& out2)
{
    const CWalletTx* pcoin1 = out1.tx;
    const CWalletTx* pcoin2 = out2.tx;

    bool found1 = false;
    bool found2 = false;
    for (int64_t d : darkSendDenominations) // loop through predefined denoms
    {
        if (pcoin1->tx->vout[out1.i].nValue == d) found1 = true;
        if (pcoin2->tx->vout[out2.i].nValue == d) found2 = true;
    }
    return (!found1 && found2);
}

bool CWallet::MintableCoins()
{
    if (GetBalance() <= stake->GetReservedBalance())
        return false;

    vector<COutput> vCoins;
    AvailableCoins(vCoins, true);

    for (const COutput& out : vCoins) {
        if (GetTime() > stake->GetStakeAge(out.tx->GetTxTime()))
            return true;
    }

    return false;
}

bool CWallet::SelectCoinsMinConf(const std::string &account, const CAmount& nTargetValue, int nConfMine, int nConfTheirs, vector<COutput> vCoins, set<pair<const CWalletTx*, unsigned int> >& setCoinsRet, CAmount& nValueRet) const
{
    setCoinsRet.clear();
    nValueRet = 0;

    // List of values less than target
    pair<CAmount, pair<const CWalletTx*, unsigned int> > coinLowestLarger;
    coinLowestLarger.first = std::numeric_limits<CAmount>::max();
    coinLowestLarger.second.first = nullptr;
    vector<pair<CAmount, pair<const CWalletTx*, unsigned int> > > vValue;
    CAmount nTotalLower = 0;

    random_shuffle(vCoins.begin(), vCoins.end(), GetRandInt);

    // move denoms down on the list
    sort(vCoins.begin(), vCoins.end(), less_then_denom);

    // try to find nondenom first to prevent unneeded spending of mixed coins
    for (unsigned int tryDenom = 0; tryDenom < 2; tryDenom++) {
        if (fDebug) LogPrint(BCLog::SELECTCOINS, "tryDenom: %d\n", tryDenom);
        vValue.clear();
        nTotalLower = 0;
        for (const COutput& output : vCoins) {
            if (!output.fSpendable)
                continue;

            const CWalletTx* pcoin = output.tx;

            //            if (fDebug) LogPrint(BCLog::SELECTCOINS, "value %s confirms %d\n", FormatMoney(pcoin->vout[output.i].nValue), output.nDepth);
            if (output.nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? nConfMine : nConfTheirs))
                continue;

            int i = output.i;
            CAmount n = pcoin->tx->vout[i].nValue;
            if (tryDenom == 0 && IsDenominatedAmount(n)) continue; // we don't want denom values on first run

            pair<CAmount, pair<const CWalletTx*, unsigned int> > coin = std::make_pair(n, std::make_pair(pcoin, i));

            if (n == nTargetValue) {
                setCoinsRet.insert(coin.second);
                nValueRet += coin.first;
                return true;
            } else if (n < nTargetValue + CENT) {
                vValue.push_back(coin);
                nTotalLower += n;
            } else if (n < coinLowestLarger.first) {
                coinLowestLarger = coin;
            }
        }

        if (nTotalLower == nTargetValue) {
            for (unsigned int i = 0; i < vValue.size(); ++i) {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
            }
            return true;
        }

        if (nTotalLower < nTargetValue) {
            if (coinLowestLarger.second.first == nullptr) // there is no input larger than nTargetValue
            {
                if (tryDenom == 0)
                    // we didn't look at denom yet, let's do it
                    continue;
                else
                    // we looked at everything possible and didn't find anything, no luck
                    return false;
            }
            setCoinsRet.insert(coinLowestLarger.second);
            nValueRet += coinLowestLarger.first;
            return true;
        }

        // nTotalLower > nTargetValue
        break;
    }

    // Solve subset sum by stochastic approximation
    sort(vValue.rbegin(), vValue.rend(), CompareValueOnly());
    vector<char> vfBest;
    CAmount nBest;

    ApproximateBestSubset(vValue, nTotalLower, nTargetValue, vfBest, nBest, 1000);
    if (nBest != nTargetValue && nTotalLower >= nTargetValue + CENT)
        ApproximateBestSubset(vValue, nTotalLower, nTargetValue + CENT, vfBest, nBest, 1000);

    // If we have a bigger coin and (either the stochastic approximation didn't find a good solution,
    //                                   or the next bigger coin is closer), return the bigger coin
    if (coinLowestLarger.second.first &&
        ((nBest != nTargetValue && nBest < nTargetValue + CENT) || coinLowestLarger.first <= nBest)) {
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
    } else {
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i]) {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
            }

        if (LogAcceptCategory(BCLog::SELECTCOINS)) {
            LogPrint(BCLog::SELECTCOINS, "CWallet::SelectCoinsMinConf best subset: ");
            for (unsigned int i = 0; i < vValue.size(); i++) {
                if (vfBest[i]) {
                    LogPrint(BCLog::SELECTCOINS, "%s ", FormatMoney(vValue[i].first));
                }
            }
            LogPrint(BCLog::SELECTCOINS, "total %s\n", FormatMoney(nBest));
        }
    }

    return true;
}

// helper, using default account (for unit tests)
bool CWallet::SelectCoinsMinConf(const CAmount& nTargetValue, int nConfMine, int nConfTheirs, vector<COutput> vCoins, set<pair<const CWalletTx*, unsigned int> >& setCoinsRet, CAmount& nValueRet) const
{
    std::string account="";
    return this->SelectCoinsMinConf(account, nTargetValue, nConfMine, nConfTheirs, vCoins, setCoinsRet, nValueRet);
}

bool CWallet::SelectCoins(const std::string &account, const CAmount& nTargetValue, set<pair<const CWalletTx*, unsigned int> >& setCoinsRet, CAmount& nValueRet, const CCoinControl* coinControl, AvailableCoinsType coin_type, bool useIX) const
{
    // Note: this function should never be used for "always free" tx types like dstx

    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl, false, coin_type, useIX);

    // coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coinControl && coinControl->HasSelected()) {
        for (const COutput& out : vCoins) {
            if (!out.fSpendable) continue;

            if (coin_type == ONLY_DENOMINATED) {
                CTxIn vin = CTxIn(out.tx->GetHash(), out.i);
                int rounds = GetInputDarkSendRounds(vin);
                // make sure it's actually anonymized
                if (rounds < nDarksendRounds) continue;
            }
            nValueRet += out.tx->tx->vout[out.i].nValue;
            setCoinsRet.insert(std::make_pair(out.tx, out.i));
            }
        return (nValueRet >= nTargetValue);
    }

    //if we're doing only denominated, we need to round up to the nearest .1 LUX
    if (coin_type == ONLY_DENOMINATED) {
        // Make outputs by looping through denominations, from large to small
        for (int64_t v : darkSendDenominations) {
            for (const COutput& out : vCoins) {
                const CTxOut& txout = out.tx->tx->vout[out.i];
                if (txout.nValue == v //make sure it's the denom we're looking for
                    && nValueRet + txout.nValue < nTargetValue + (0.1 * COIN) + 100 //round the amount up to .1 LUX over
                    ) {

                    CTxIn vin = CTxIn(out.tx->GetHash(), out.i);
                    int rounds = GetInputDarkSendRounds(vin);
                    if (rounds < nDarksendRounds) // make sure it's actually anonymized
                        continue;
                     nValueRet += out.tx->tx->vout[out.i].nValue;
                    setCoinsRet.insert(std::make_pair(out.tx, out.i));
                    }
                }
            }
        return (nValueRet >= nTargetValue);
    }

    return (SelectCoinsMinConf(account, nTargetValue, 1, 6, vCoins, setCoinsRet, nValueRet) ||
            SelectCoinsMinConf(account, nTargetValue, 1, 1, vCoins, setCoinsRet, nValueRet) ||
            (bSpendZeroConfChange && SelectCoinsMinConf(account, nTargetValue, 0, 1, vCoins, setCoinsRet, nValueRet)));
}

struct CompareByPriority {
    bool operator()(const COutput& t1,
        const COutput& t2) const
    {
        return t1.Priority() > t2.Priority();
    }
};

bool CWallet::SelectCoinsByDenominations(int nDenom, int64_t nValueMin, int64_t nValueMax, std::vector<CTxIn>& vCoinsRet, std::vector<COutput>& vCoinsRet2, int64_t& nValueRet, int nDarksendRoundsMin, int nDarksendRoundsMax)
{
    vCoinsRet.clear();
    nValueRet = 0;

    vCoinsRet2.clear();
    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, nullptr, false, ONLY_DENOMINATED);

    std::random_shuffle(vCoins.rbegin(), vCoins.rend());

    //keep track of each denomination that we have
    bool fFound10000 = false;
    bool fFound1000 = false;
    bool fFound100 = false;
    bool fFound10 = false;
    bool fFound1 = false;
    bool fFoundDot1 = false;

    //Check to see if any of the denomination are off, in that case mark them as fulfilled
    if (!(nDenom & (1 << 0))) fFound10000 = true;
    if (!(nDenom & (1 << 1))) fFound1000 = true;
    if (!(nDenom & (1 << 2))) fFound100 = true;
    if (!(nDenom & (1 << 3))) fFound10 = true;
    if (!(nDenom & (1 << 4))) fFound1 = true;
    if (!(nDenom & (1 << 5))) fFoundDot1 = true;

    for (const COutput& out : vCoins) {
        // masternode-like input should not be selected by AvailableCoins now anyway
        //if(out.tx->vout[out.i].nValue == DARKSEND_COLLATERAL) continue;
        if (nValueRet + out.tx->tx->vout[out.i].nValue <= nValueMax) {
            bool fAccepted = false;

            // Function returns as follows:
            //
            // bit 0 - 10000 LUX+1 ( bit on if present )
            // bit 1 - 1000 LUX+1
            // bit 2 - 100 LUX+1
            // bit 3 - 10 LUX+1
            // bit 4 - 1 LUX+1
            // bit 5 - .1 LUX+1

            CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

            int rounds = GetInputDarkSendRounds(vin);
            if (rounds >= nDarksendRoundsMax) continue;
            if (rounds < nDarksendRoundsMin) continue;

            if (fFound10000 && fFound1000 && fFound100 && fFound10 && fFound1 && fFoundDot1) { //if fulfilled
                //we can return this for submission
                if (nValueRet >= nValueMin) {
                    //random reduce the max amount we'll submit for anonymity
                    nValueMax -= (rand() % (nValueMax / 5));
                    //on average use 50% of the inputs or less
                    int r = (rand() % (int)vCoins.size());
                    if ((int)vCoinsRet.size() > r) return true;
                }
                //Denomination criterion has been met, we can take any matching denominations
                if ((nDenom & (1 << 0)) && out.tx->tx->vout[out.i].nValue == ((10000 * COIN) + 10000000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 1)) && out.tx->tx->vout[out.i].nValue == ((1000 * COIN) + 1000000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 2)) && out.tx->tx->vout[out.i].nValue == ((100 * COIN) + 100000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 3)) && out.tx->tx->vout[out.i].nValue == ((10 * COIN) + 10000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 4)) && out.tx->tx->vout[out.i].nValue == ((1 * COIN) + 1000)) {
                    fAccepted = true;
                } else if ((nDenom & (1 << 5)) && out.tx->tx->vout[out.i].nValue == ((.1 * COIN) + 100)) {
                    fAccepted = true;
                }
            } else {
                //Criterion has not been satisfied, we will only take 1 of each until it is.
                if ((nDenom & (1 << 0)) && out.tx->tx->vout[out.i].nValue == ((10000 * COIN) + 10000000)) {
                    fAccepted = true;
                    fFound10000 = true;
                } else if ((nDenom & (1 << 1)) && out.tx->tx->vout[out.i].nValue == ((1000 * COIN) + 1000000)) {
                    fAccepted = true;
                    fFound1000 = true;
                } else if ((nDenom & (1 << 2)) && out.tx->tx->vout[out.i].nValue == ((100 * COIN) + 100000)) {
                    fAccepted = true;
                    fFound100 = true;
                } else if ((nDenom & (1 << 3)) && out.tx->tx->vout[out.i].nValue == ((10 * COIN) + 10000)) {
                    fAccepted = true;
                    fFound10 = true;
                } else if ((nDenom & (1 << 4)) && out.tx->tx->vout[out.i].nValue == ((1 * COIN) + 1000)) {
                    fAccepted = true;
                    fFound1 = true;
                } else if ((nDenom & (1 << 5)) && out.tx->tx->vout[out.i].nValue == ((.1 * COIN) + 100)) {
                    fAccepted = true;
                    fFoundDot1 = true;
                }
            }
            if (!fAccepted) continue;

            vin.prevPubKey = out.tx->tx->vout[out.i].scriptPubKey; // the inputs PubKey
            nValueRet += out.tx->tx->vout[out.i].nValue;
            vCoinsRet.push_back(vin);
            vCoinsRet2.push_back(out);
        }
    }

    return (nValueRet >= nValueMin && fFound10000 && fFound1000 && fFound100 && fFound10 && fFound1 && fFoundDot1);
}

bool CWallet::SelectCoinsDark(CAmount nValueMin, CAmount nValueMax, std::vector<CTxIn>& setCoinsRet, CAmount& nValueRet, int nDarksendRoundsMin, int nDarksendRoundsMax) const
{
    CCoinControl* coinControl = nullptr;

    setCoinsRet.clear();
    nValueRet = 0;

    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl, false, nDarksendRoundsMin < 0 ? ONLY_NONDENOMINATED_NOTMN : ONLY_DENOMINATED);

    set<pair<const CWalletTx*, unsigned int> > setCoinsRet2;

    //order the array so largest nondenom are first, then denominations, then very small inputs.
    sort(vCoins.rbegin(), vCoins.rend(), CompareByPriority());

    //the first thing we get is a fee input, then we'll use as many denominated as possible. then the rest
    for (const COutput& out : vCoins) {
        //there's no reason to allow inputs less than 1 COIN into DS (other than denominations smaller than that amount)
        if (out.tx->tx->vout[out.i].nValue < 1*COIN && out.tx->tx->vout[out.i].nValue != (.1*COIN)+100) continue;
        if (fMasterNode && out.tx->tx->vout[out.i].nValue == GetMNCollateral(chainActive.Height()) * COIN) continue; //masternode input

        if (nValueRet + out.tx->tx->vout[out.i].nValue <= nValueMax) {
            CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

            int rounds = GetInputDarksendRounds(vin);
            if (rounds >= nDarksendRoundsMax) continue;
            if (rounds < nDarksendRoundsMin) continue;

            vin.prevPubKey = out.tx->tx->vout[out.i].scriptPubKey; // the inputs PubKey
            nValueRet += out.tx->tx->vout[out.i].nValue;
            setCoinsRet.push_back(vin);
            setCoinsRet2.insert(std::make_pair(out.tx, out.i));
        }
    }

    // if it's more than min, we're good to return
    if (nValueRet >= nValueMin) return true;

    return false;
}

bool CWallet::SelectCoinsCollateral(std::vector<CTxIn>& setCoinsRet, int64_t& nValueRet) const
{
    vector<COutput> vCoins;

    //LogPrintf(" selecting coins for collateral\n");
    AvailableCoins(vCoins);

    //LogPrintf("found coins %d\n", (int)vCoins.size());

    set<pair<const CWalletTx*, unsigned int> > setCoinsRet2;

    for (const COutput& out : vCoins) {
        // collateral inputs will always be a multiple of DARSEND_COLLATERAL, up to five
        if (IsCollateralAmount(out.tx->tx->vout[out.i].nValue)) {
            CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

            vin.prevPubKey = out.tx->tx->vout[out.i].scriptPubKey; // the inputs PubKey
            nValueRet += out.tx->tx->vout[out.i].nValue;
            setCoinsRet.push_back(vin);
            setCoinsRet2.insert(std::make_pair(out.tx, out.i));
            return true;
        }
    }

    return false;
}

int CWallet::CountInputsWithAmount(int64_t nInputAmount)
{
    int64_t nTotal = 0;
    {
        LOCK(cs_wallet);
        for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted()) {
                int nDepth = pcoin->GetDepthInMainChain(false);

                for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
                    COutput out = COutput(pcoin, i, nDepth, true);
                    CTxIn vin = CTxIn(out.tx->tx->GetHash(), out.i);

                    if (out.tx->tx->vout[out.i].nValue != nInputAmount) continue;
                    if (!IsDenominatedAmount(pcoin->tx->vout[i].nValue)) continue;
                    if (IsSpent(out.tx->tx->GetHash(), i) || IsMine(pcoin->tx->vout[i]) != ISMINE_SPENDABLE || !IsDenominated(vin)) continue;

                    nTotal++;
                }
            }
        }
    }

    return nTotal;
}

bool CWallet::HasCollateralInputs(bool fOnlyConfirmed) const
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins, fOnlyConfirmed);

    int nFound = 0;
    for (const COutput& out : vCoins)
        if (IsCollateralAmount(out.tx->tx->vout[out.i].nValue)) nFound++;

    return nFound > 0;
}

bool CWallet::IsCollateralAmount(int64_t nInputAmount) const
{
    return  nInputAmount == (DARKSEND_COLLATERAL * 5)+DARKSEND_FEE ||
            nInputAmount == (DARKSEND_COLLATERAL * 4)+DARKSEND_FEE ||
            nInputAmount == (DARKSEND_COLLATERAL * 3)+DARKSEND_FEE ||
            nInputAmount == (DARKSEND_COLLATERAL * 2)+DARKSEND_FEE ||
            nInputAmount == (DARKSEND_COLLATERAL * 1)+DARKSEND_FEE;
}

bool CWallet::CreateCollateralTransaction(CMutableTransaction& txCollateral, std::string& strReason)
{
    /*
        To doublespend a collateral transaction, it will require a fee higher than this. So there's
        still a significant cost.
    */
    CAmount nFeeRet = 1 * COIN;

    txCollateral.vin.clear();
    txCollateral.vout.clear();

    CReserveKey reservekey(this);
    CAmount nValueIn2 = 0;
    std::vector<CTxIn> vCoinsCollateral;

    if (!SelectCoinsCollateral(vCoinsCollateral, nValueIn2)) {
        strReason = "Error: DarkSend requires a collateral transaction and could not locate an acceptable input!";
        return false;
    }

    // make our change address
    CScript scriptChange;
    CPubKey vchPubKey;
    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptChange = GetScriptForDestination(vchPubKey.GetID());
    reservekey.KeepKey();

    for (CTxIn v : vCoinsCollateral)
        txCollateral.vin.push_back(v);

    if (nValueIn2 - DARKSEND_COLLATERAL - nFeeRet > 0) {
        //pay collateral charge in fees
        CTxOut vout3 = CTxOut(nValueIn2 - DARKSEND_COLLATERAL, scriptChange);
        txCollateral.vout.push_back(vout3);
    }

    int vinNumber = 0;
    for (CTxIn v : txCollateral.vin) {
        CTransactionRef txPrev;
        uint256 prevBlockHash;
        //Find previous transaction with the same output as txNew input
        if (!GetTransaction(v.prevout.hash, txPrev, Params().GetConsensus(), prevBlockHash)) {
            strReason = "CDarkSendPool::MutateTxSign() - Signing - Failed to get previous transaction\n";
            return false;
        }
        const CAmount& amount = txPrev->vout[v.prevout.n].nValue;
        if (!SignSignature(*this, v.prevPubKey, txCollateral, vinNumber, amount, int(SIGHASH_ALL | SIGHASH_ANYONECANPAY))) {
            for (CTxIn v : vCoinsCollateral)
                UnlockCoin(v.prevout);

            strReason = "CDarkSendPool::Sign - Unable to sign collateral transaction! \n";
            return false;
        }
        vinNumber++;
    }

    return true;
}

bool CWallet::ConvertList(std::vector<CTxIn> vCoins, std::vector<int64_t>& vecAmounts)
{
    for (CTxIn i : vCoins) {
        if (mapWallet.count(i.prevout.hash)) {
            CWalletTx& wtx = mapWallet[i.prevout.hash];
            if (i.prevout.n < wtx.tx->vout.size()) {
                vecAmounts.push_back(wtx.tx->vout[i.prevout.n].nValue);
            }
        } else {
            LogPrintf("ConvertList -- Couldn't find transaction\n");
        }
    }
    return true;
}

OutputType CWallet::TransactionChangeType(const std::vector<std::pair<CScript, CAmount> >& vecSend)
{
    // If -changetype is specified, always use that change type.
    if (g_change_type != OUTPUT_TYPE_NONE) {
        return g_change_type;
    }

    // if g_address_type is legacy, use legacy address as change (even
    // if some of the outputs are P2WPKH or P2WSH).
    if (g_address_type == OUTPUT_TYPE_LEGACY) {
        return OUTPUT_TYPE_LEGACY;
    }

    // if any destination is P2WPKH or P2WSH, use P2WPKH for the change
    // output.
    for (const auto& recipient : vecSend) {
        // Check if any destination contains a witness program:
        int witnessversion = 0;
        std::vector<unsigned char> witnessprogram;
        if (recipient.first.IsWitnessProgram(witnessversion, witnessprogram)) {
            return OUTPUT_TYPE_BECH32;
        }
    }

    // else use g_address_type for change
    return g_address_type;
}

bool CWallet::CreateTransaction(const vector<pair<CScript, CAmount> >& vecSend, CWalletTx& wtxNew, CReserveKey& reservekey, CAmount& nFeeRet, std::string& strFailReason, const CCoinControl* coinControl, AvailableCoinsType coin_type, bool useIX, CAmount nFeePay, CAmount nGasFee)
{
#   if defined(DEBUG_DUMP_STAKING_INFO) && defined(DEBUG_DUMP_CreateTransaction_1)
    DEBUG_DUMP_CreateTransaction_1();
#   endif

    if (useIX && nFeePay < CENT) nFeePay = CENT;

    CAmount nValue = 0;

    for (const PAIRTYPE(CScript, CAmount) & s : vecSend) {
        if (nValue < 0 || s.second < 0) {
            strFailReason = _("Transaction amounts must be positive");
            return false;
        }
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0) {
        strFailReason = _("Transaction amounts must be positive");
        return false;
    }

    wtxNew.fTimeReceivedIsTxTime = true;
    wtxNew.BindWallet(this);
    CMutableTransaction txNew;

    {
        LOCK2(cs_main, cs_wallet);
        {
            nFeeRet = 0;
            if (nFeePay > 0) nFeeRet = nFeePay;
            while (true) {
                txNew.vin.clear();
                txNew.vout.clear();
                wtxNew.fFromMe = true;

                CAmount nTotalValue = nValue + nFeeRet;
                double dPriority = 0;

                // vouts to the payees
                if (coinControl && !coinControl->fSplitBlock) {
                    for (const PAIRTYPE(CScript, CAmount) & s : vecSend) {
                        CTxOut txout(s.second, s.first);
                        if (txout.IsDust(::minRelayTxFee)) {
                            strFailReason = _("Transaction amount too small");
                            return false;
                        }
                        txNew.vout.push_back(txout);
                    }
                } else { //UTXO Splitter Transaction
                    int nSplitBlock;

                    if (coinControl)
                        nSplitBlock = coinControl->nSplitBlock;
                    else
                        nSplitBlock = 1;

                    for (const PAIRTYPE(CScript, CAmount) & s : vecSend) {
                        for (int i = 0; i < nSplitBlock; i++) {
                            if (i == nSplitBlock - 1) {
                                uint64_t nRemainder = s.second % nSplitBlock;
                                txNew.vout.push_back(CTxOut((s.second / nSplitBlock) + nRemainder, s.first));
                            } else
                                txNew.vout.push_back(CTxOut(s.second / nSplitBlock, s.first));
                        }
                    }
                }

                // Choose coins to use
                set<pair<const CWalletTx*, unsigned int> > setCoins;
                CAmount nValueIn = 0;

                if (!SelectCoins(wtxNew.strFromAccount, nTotalValue, setCoins, nValueIn, coinControl, coin_type, useIX)) {
                    if (coin_type == ALL_COINS) {
                        strFailReason = _("Insufficient funds.");
                    } else if (coin_type == ONLY_NONDENOMINATED) {
                        strFailReason = _("Unable to locate enough funds for this transaction that are not equal 10000 LUX.");
                    } else if (coin_type == ONLY_NONDENOMINATED_NOTMN) {
                        strFailReason = _("Unable to locate enough DarkSend non-denominated funds for this transaction that are not equal 10000 LUX.");
                    } else {
                        strFailReason = _("Unable to locate enough DarkSend denominated funds for this transaction.");
                        strFailReason += " " + _("DarkSend uses exact denominated amounts to send funds, you might simply need to anonymize some more coins.");
                    }

                    if (useIX) {
                        strFailReason += " " + _("InstantX requires inputs with at least 6 confirmations, you might need to wait a few minutes and try again.");
                    }

                    return false;
                }

#               if defined(DEBUG_DUMP_STAKING_INFO) && defined(DEBUG_DUMP_CreateTransaction_2)
                DEBUG_DUMP_CreateTransaction_2();
#               endif

                for (PAIRTYPE(const CWalletTx*, unsigned int) pcoin : setCoins) {
                    CAmount nCredit = pcoin.first->tx->vout[pcoin.second].nValue;
                    //The coin age after the next block (depth+1) is used instead of the current,
                    //reflecting an assumption the user would accept a bit more delay for
                    //a chance at a free transaction.
                    //But mempool inputs might still be in the mempool, so their age stays 0
                    int age = pcoin.first->GetDepthInMainChain();
                    if (age != 0)
                        age += 1;
                    dPriority += (double)nCredit * age;
                }

                CAmount nChange = nValueIn - nValue - nFeeRet;

                //over pay for denominated transactions
                if (coin_type == ONLY_DENOMINATED) {
                    nFeeRet += nChange;
                    nChange = 0;
                    wtxNew.mapValue["DS"] = "1";
                }

                if (nChange > 0) {
                    // Fill a vout to ourself
                    // TODO: pass in scriptChange instead of reservekey so
                    // change transaction isn't always pay-to-lux-address
                    CScript scriptChange;

                    bool combineChange = false;

                    // coin control: send change to custom address
                    if (coinControl && !boost::get<CNoDestination>(&coinControl->destChange))
                    {
                        scriptChange = GetScriptForDestination(coinControl->destChange);

                        vector<CTxOut>::iterator it = txNew.vout.begin();
                        while (it != txNew.vout.end())
                        {
                            if (scriptChange == it->scriptPubKey)
                            {
                                it->nValue += nChange;
                                nChange = 0;
                                reservekey.ReturnKey();
                                combineChange = true;
                                break;
                            }
                            ++it;
                        }
                    }
                    // no coin control: send change to newly generated address
                    else {
                        // Note: We use a new key here to keep it from being obvious which side is the change.
                        //  The drawback is that by not reusing a previous key, the change may be lost if a
                        //  backup is restored, if the backup doesn't have the new private key for the change.
                        //  If we reused the old key, it would be possible to add code to look for and
                        //  rediscover unknown transactions that were written with keys of ours to recover
                        //  post-backup change.

                        // Reserve a new key pair from key pool
                        if (IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                            strFailReason = _("Can't generate a change-address key. Private keys are disabled for this wallet.");
                            return false;
                        }
                        CPubKey vchPubKey;
                        bool ret;
                        ret = reservekey.GetReservedKey(vchPubKey, true);
                        if (!ret)
                        {
                            strFailReason = _("Keypool ran out, please call keypoolrefill first");
                            return false;
                        }

                        const OutputType change_type = TransactionChangeType(vecSend);

                        LearnRelatedScripts(vchPubKey, change_type);
                        scriptChange = GetScriptForDestination(GetDestinationForKey(vchPubKey, change_type));
                    }

                    if (!combineChange) {

                        CTxOut newTxOut(nChange, scriptChange);

                        // Never create dust outputs; if we would, just
                        // add the dust to the fee.
                        if (newTxOut.IsDust(::minRelayTxFee)) {
                            nFeeRet += nChange;
                            nChange = 0;
                            reservekey.ReturnKey();
                        } else {
                            // Insert change txn at random position:
                            vector<CTxOut>::iterator position = txNew.vout.begin() + GetRandInt(txNew.vout.size() + 1);
                            txNew.vout.insert(position, newTxOut);
                        }
                    }
                } else
                    reservekey.ReturnKey();

                // Fill vin
                for (const PAIRTYPE(const CWalletTx*, unsigned int) & coin : setCoins)
                    txNew.vin.push_back(CTxIn(coin.first->GetHash(), coin.second));

                // Sign
                int nIn = 0;
                CTransaction txNewConst(txNew);
                for (const PAIRTYPE(const CWalletTx*, unsigned int) & coin : setCoins) {
                    bool signSuccess;
                    const CScript& scriptPubKey = coin.first->tx->vout[coin.second].scriptPubKey;
                    SignatureData sigdata;
                    signSuccess = ProduceSignature(TransactionSignatureCreator(this, &txNewConst, nIn, coin.first->tx->vout[coin.second].nValue, SIGHASH_ALL), scriptPubKey, sigdata);
                    if (!signSuccess) {
                        strFailReason = _("Signing transaction failed");
                        return false;
                    } else {
                        UpdateTransaction(txNew, nIn, sigdata);
                    }
                    nIn++;
                }

                // Embed the constructed transaction data in wtxNew.
                wtxNew.SetTx(MakeTransactionRef(std::move(txNew)));

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*wtxNew.tx, SER_NETWORK, PROTOCOL_VERSION);
                if (nBytes >= MAX_STANDARD_TX_SIZE) {
                    strFailReason = _("Transaction too large");
                    return false;
                }
                dPriority = wtxNew.ComputePriority(dPriority, nBytes);

                // Can we complete this as a free transaction?
                if (fSendFreeTransactions && nBytes <= MAX_FREE_TRANSACTION_CREATE_SIZE) {
                    // Not enough fee: enough priority?
                    double dPriorityNeeded = mempool.estimateSmartPriority(nTxConfirmTarget);
                    // Not enough mempool history to estimate: use hard-coded AllowFree.
                    if (dPriorityNeeded <= 0 && AllowFree(dPriority))
                        break;

                    // Small enough, and priority high enough, to send for free
                    if (dPriorityNeeded > 0 && dPriority >= dPriorityNeeded)
                        break;
                }

                CAmount nFeeNeeded = max(nFeePay, GetMinimumFee(nBytes, nTxConfirmTarget, mempool)) + nGasFee;

                // If we made it here and we aren't even able to meet the relay fee on the next pass, give up
                // because we must be at the maximum allowed fee.
                if (nFeeNeeded < ::minRelayTxFee.GetFee(nBytes)) {
                    strFailReason = _("Transaction too large for fee policy");
                    return false;
                }

                if (nFeeRet >= nFeeNeeded) // Done, enough fee included
                    break;

                // Include more fee and try again.
                nFeeRet = nFeeNeeded;
                continue;
            }
        }
    }
    return true;
}

bool CWallet::CreateTransaction(CScript scriptPubKey, const CAmount& nValue, CWalletTx& wtxNew, CReserveKey& reservekey, CAmount& nFeeRet, std::string& strFailReason, const CCoinControl* coinControl, AvailableCoinsType coin_type, bool useIX, CAmount nFeePay)
{
    vector<pair<CScript, CAmount> > vecSend;
    vecSend.push_back(std::make_pair(scriptPubKey, nValue));
    return CreateTransaction(vecSend, wtxNew, reservekey, nFeeRet, strFailReason, coinControl, coin_type, useIX, nFeePay);
}

/**
 * Call after CreateTransaction unless you want to abort
 */
bool CWallet::CommitTransaction(CWalletTx& wtxNew, CReserveKey& reservekey, std::string strCommand)
{
    {
        LOCK2(cs_main, cs_wallet);
#       if defined(DEBUG_DUMP_STAKING_INFO)&&defined(DEBUG_DUMP_CommitTransaction)
        DEBUG_DUMP_CommitTransaction();
#       endif
        {
            // This is only to keep the database open to defeat the auto-flush for the
            // duration of this scope.  This is the only place where this optimization
            // maybe makes sense; please don't do it anywhere else.
            CWalletDB* pwalletdb = fFileBacked ? new CWalletDB(strWalletFile, "r") : nullptr;

            // Take key pair from key pool so it won't be used again
            reservekey.KeepKey();

            // Add tx to wallet, because if it has change it's also ours,
            // otherwise just for transaction history.
            AddToWallet(wtxNew);

            // Notify that old coins are spent
            set<uint256> updated_hahes;
            for (const CTxIn& txin : wtxNew.tx->vin) {
                // notify only once
                if (updated_hahes.find(txin.prevout.hash) != updated_hahes.end()) continue;

                CWalletTx& coin = mapWallet[txin.prevout.hash];
                coin.BindWallet(this);
                NotifyTransactionChanged(this, txin.prevout.hash, CT_UPDATED);
                updated_hahes.insert(txin.prevout.hash);
            }
            if (fFileBacked)
                delete pwalletdb;
        }

        // Track how many getdata requests our transaction gets
        mapRequestCount[wtxNew.GetHash()] = 0;

        if (fBroadcastTransactions) {
            // Broadcast
            if (!wtxNew.AcceptToMemoryPool(false)) {
                // This must not fail. The transaction has already been signed and recorded.
                LogPrintf("CommitTransaction() : Error: Transaction not valid %s\n", strCommand.c_str());
                return false;
            }
            wtxNew.RelayWalletTransaction(strCommand);
        }
    }
    return true;
}

void CWallet::ListAccountCreditDebit(const std::string& strAccount, std::list<CAccountingEntry>& entries) {
    CWalletDB walletdb(strWalletFile);
    return walletdb.ListAccountCreditDebit(strAccount, entries);
}

bool CWallet::AddAccountingEntry(const CAccountingEntry& acentry)
{
    CWalletDB walletdb(strWalletFile);

    return AddAccountingEntry(acentry, &walletdb);
}

bool CWallet::AddAccountingEntry(const CAccountingEntry& acentry, CWalletDB *pwalletdb)
{
    if (!pwalletdb->WriteAccountingEntry(++nAccountingEntryNumber, acentry)) {
        return false;
    }

    laccentries.push_back(acentry);
    CAccountingEntry & entry = laccentries.back();
    wtxOrdered.insert(std::make_pair(entry.nOrderPos, TxPair((CWalletTx*)0, &entry)));

    return true;
}

CAmount CWallet::GetMinimumFee(unsigned int nTxBytes, unsigned int nConfirmTarget, const CTxMemPool& pool)
{
    // payTxFee is user-set "I want to pay this much"
    CAmount nFeeNeeded = payTxFee.GetFee(nTxBytes);
    // user selected total at least (default=true)
    if (fPayAtLeastCustomFee && nFeeNeeded > 0 && nFeeNeeded < payTxFee.GetFeePerK())
        nFeeNeeded = payTxFee.GetFeePerK();
    // User didn't set: use -txconfirmtarget to estimate...
    if (nFeeNeeded == 0) {
        int estimateFoundTarget = nConfirmTarget;
        nFeeNeeded = pool.estimateSmartFee(nConfirmTarget, &estimateFoundTarget).GetFee(nTxBytes);
    }
    // ... unless we don't have enough mempool data, in which case fall
    // back to a hard-coded fee
    if (nFeeNeeded == 0)
        nFeeNeeded = minTxFee.GetFee(nTxBytes);
    // prevent user from paying a non-sense fee (like 1 satoshi): 0 < fee < minRelayFee
    if (nFeeNeeded < ::minRelayTxFee.GetFee(nTxBytes))
        nFeeNeeded = ::minRelayTxFee.GetFee(nTxBytes);
    // But always obey the maximum
    if (nFeeNeeded > maxTxFee)
        nFeeNeeded = maxTxFee;
    return nFeeNeeded;
}

int64_t CWallet::GetTotalValue(std::vector<CTxIn> vCoins)
{
    int64_t nTotalValue = 0;
    CWalletTx wtx;
    for (CTxIn i : vCoins) {
        if (mapWallet.count(i.prevout.hash)) {
            CWalletTx& wtx = mapWallet[i.prevout.hash];
            if (i.prevout.n < wtx.tx->vout.size()) {
                nTotalValue += wtx.tx->vout[i.prevout.n].nValue;
            }
        } else {
            LogPrintf("GetTotalValue -- Couldn't find transaction\n");
        }
    }
    return nTotalValue;
}

std::string CWallet::PrepareDarksendDenominate(int minRounds, int maxRounds)
{
    if (IsLocked())
        return _("Error: Wallet locked, unable to create transaction!");

    if(darkSendPool.GetState() != POOL_STATUS_ERROR && darkSendPool.GetState() != POOL_STATUS_SUCCESS)
        if(darkSendPool.GetMyTransactionCount() > 0)
            return _("Error: You already have pending entries in the Darksend pool");

    // ** find the coins we'll use
    std::vector<CTxIn> vCoins;
    std::vector<CTxIn> vCoinsResult;
    std::vector<COutput> vCoins2;
    int64_t nValueIn = 0;
    CReserveKey reservekey(this);

    /*
        Select the coins we'll use
        if minRounds >= 0 it means only denominated inputs are going in and coming out
    */
    if(minRounds >= 0){
        if (!SelectCoinsByDenominations(darkSendPool.sessionDenom, 0.1*COIN, DARKSEND_POOL_MAX, vCoins, vCoins2, nValueIn, minRounds, maxRounds))
            return _("Error: can't select current denominated inputs");
    }

    // calculate total value out
    LogPrintf("PrepareDarksendDenominate - preparing darksend denominate . Got: %d \n", nValueIn);


    //--------------
    for (CTxIn v : vCoins)
                    LockCoin(v.prevout);

    int64_t nValueLeft = nValueIn;
    std::vector<CTxOut> vOut;

    /*
        TODO: Front load with needed denominations (e.g. .1, 1 )
    */

    /*
       Add all denominations once
       The beginning of the list is front loaded with each possible
       denomination in random order. This means we'll at least get 1
       of each that is required as outputs.
   */
    int nStep = 0;
    int nStepsMax = 5 + GetRandInt(5);
    while(nStep < nStepsMax) {

        for (int64_t v : darkSendDenominations){
            // only use the ones that are approved
            bool fAccepted = false;
            if((darkSendPool.sessionDenom & (1 << 0))      && v == ((100*COIN) +100000)) {fAccepted = true;}
            else if((darkSendPool.sessionDenom & (1 << 1)) && v == ((10*COIN)  +10000)) {fAccepted = true;}
            else if((darkSendPool.sessionDenom & (1 << 2)) && v == ((1*COIN)   +1000)) {fAccepted = true;}
            else if((darkSendPool.sessionDenom & (1 << 3)) && v == ((.1*COIN)  +100)) {fAccepted = true;}
            if(!fAccepted) continue;

            // try to add it
            if(nValueLeft - v >= 0) {
                // Note: this relies on a fact that both vectors MUST have same size
                std::vector<CTxIn>::iterator it = vCoins.begin();
                std::vector<COutput>::iterator it2 = vCoins2.begin();
                while(it2 != vCoins2.end()) {
                    // we have matching inputs
                    if((*it2).tx->tx->vout[(*it2).i].nValue == v) {
                        // add new input in resulting vector
                        vCoinsResult.push_back(*it);
                        // remove corresponting items from initial vectors
                        vCoins.erase(it);
                        vCoins2.erase(it2);

                        CScript scriptChange;
                        CPubKey vchPubKey;
                        // use a unique change address
                        assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
                        scriptChange = GetScriptForDestination(vchPubKey.GetID());
                        reservekey.KeepKey();

                        // add new output
                        CTxOut o(v, scriptChange);
                        vOut.push_back(o);

                        // subtract denomination amount
                        nValueLeft -= v;

                        break;
                    }
                    ++it;
                    ++it2;
                }
            }
        }

        nStep++;

        if(nValueLeft == 0) break;
    }

    // unlock unused coins
    for (CTxIn v : vCoins)
        UnlockCoin(v.prevout);

    if(darkSendPool.GetDenominations(vOut) != darkSendPool.sessionDenom) {
        // unlock used coins on failure
        for (CTxIn v : vCoinsResult)
            UnlockCoin(v.prevout);
        return "Error: can't make current denominated outputs";
    }

    //randomize the output order
    std::random_shuffle (vOut.begin(), vOut.end());

    // We also do not care about full amount as long as we have right denominations, just pass what we found
    darkSendPool.SendDarksendDenominate(vCoinsResult, vOut, nValueIn - nValueLeft);

    return "";
}


DBErrors CWallet::LoadWallet(bool& fFirstRunRet)
{
    fFirstRunRet = false;
    DBErrors nLoadWalletRet = CWalletDB(strWalletFile,"cr+").LoadWallet(this);
    if (nLoadWalletRet == DB_NEED_REWRITE)
    {
        if (CDB::Rewrite("\x04pool"))
        {
            LOCK(cs_wallet);
            setInternalKeyPool.clear();
            setExternalKeyPool.clear();
            m_pool_key_to_index.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // that requires a new key.
        }
    }

    if (nLoadWalletRet != DB_LOAD_OK)
        return nLoadWalletRet;
    fFirstRunRet = !vchDefaultKey.IsValid();

    uiInterface.LoadWallet(this);

    return DB_LOAD_OK;
}


DBErrors CWallet::ZapSelectTx(std::vector<uint256>& vHashIn, std::vector<uint256>& vHashOut)
{
    AssertLockHeld(cs_wallet); // mapWallet
    vchDefaultKey = CPubKey();
    DBErrors nZapSelectTxRet = CWalletDB(strWalletFile,"cr+").ZapSelectTx(vHashIn, vHashOut);
    for (uint256 hash : vHashOut)
        mapWallet.erase(hash);

    if (nZapSelectTxRet == DB_NEED_REWRITE) {
        if (CDB::Rewrite("\x04pool"))
        {
            setInternalKeyPool.clear();
            setExternalKeyPool.clear();
            m_pool_key_to_index.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // that requires a new key.
        }
    }

    if (nZapSelectTxRet != DB_LOAD_OK)
        return nZapSelectTxRet;

    MarkDirty();

    return DB_LOAD_OK;

}

DBErrors CWallet::ZapWalletTx(std::vector<CWalletTx>& vWtx)
{
    vchDefaultKey = CPubKey();
    DBErrors nZapWalletTxRet = CWalletDB(strWalletFile,"cr+").ZapWalletTx(vWtx);
    if (nZapWalletTxRet == DB_NEED_REWRITE)
    {
        if (CDB::Rewrite("\x04pool"))
        {
            LOCK(cs_wallet);
            setInternalKeyPool.clear();
            setExternalKeyPool.clear();
            m_pool_key_to_index.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // that requires a new key.
        }
    }

    if (nZapWalletTxRet != DB_LOAD_OK)
        return nZapWalletTxRet;

    return DB_LOAD_OK;
}

bool CWallet::SetAddressBook(const CTxDestination& address, const std::string& strName, const std::string& strPurpose)
{
    bool fUpdated = false;
    {
        LOCK(cs_wallet); // mapAddressBook
        std::map<CTxDestination, CAddressBookData>::iterator mi = mapAddressBook.find(address);
        fUpdated = mi != mapAddressBook.end();
        mapAddressBook[address].name = strName;
        if (!strPurpose.empty()) /* update purpose only if requested */
            mapAddressBook[address].purpose = strPurpose;
    }
    NotifyAddressBookChanged(this, address, strName, ::IsMine(*this, address) != ISMINE_NO,
        strPurpose, (fUpdated ? CT_UPDATED : CT_NEW));
    if (!fFileBacked)
        return false;
    if (!strPurpose.empty() && !CWalletDB(strWalletFile).WritePurpose(EncodeDestination(address), strPurpose))
        return false;
    return CWalletDB(strWalletFile).WriteName(EncodeDestination(address), strName);
}

bool CWallet::DelAddressBook(const CTxDestination& address)
{
    {
        LOCK(cs_wallet); // mapAddressBook

        if (fFileBacked) {
            // Delete destdata tuples associated with address
            std::string strAddress = EncodeDestination(address);
            for (const PAIRTYPE(string, string) & item : mapAddressBook[address].destdata) {
                CWalletDB(strWalletFile).EraseDestData(strAddress, item.first);
            }
        }
        mapAddressBook.erase(address);
    }

    NotifyAddressBookChanged(this, address, "", ::IsMine(*this, address) != ISMINE_NO, "", CT_DELETED);

    if (!fFileBacked)
        return false;
    CWalletDB(strWalletFile).ErasePurpose(EncodeDestination(address));
    return CWalletDB(strWalletFile).EraseName(EncodeDestination(address));
}

const std::string& CWallet::GetAccountName(const CScript& scriptPubKey) const
{
    CTxDestination address;
    if (ExtractDestination(scriptPubKey, address) && !scriptPubKey.IsUnspendable()) {
        auto mi = mapAddressBook.find(address);
        if (mi != mapAddressBook.end()) {
            return mi->second.name;
        }
    }
    // A scriptPubKey that doesn't have an entry in the address book is
    // associated with the default account ("").
    const static std::string DEFAULT_ACCOUNT_NAME;
    return DEFAULT_ACCOUNT_NAME;
}

bool CWallet::SetDefaultKey(const CPubKey& vchPubKey)
{
    if (fFileBacked) {
        if (!CWalletDB(strWalletFile).WriteDefaultKey(vchPubKey))
            return false;
    }
    vchDefaultKey = vchPubKey;
    return true;
}

/**
 * Mark old keypool keys as used,
 * and generate all new keys
 */
bool CWallet::NewKeyPool()
{
    if (IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        return false;
    }
    {
        LOCK(cs_wallet);
        CWalletDB walletdb(strWalletFile);

        for (int64_t nIndex : setInternalKeyPool) {
            walletdb.ErasePool(nIndex);
        }
        setInternalKeyPool.clear();

        for (int64_t nIndex : setExternalKeyPool) {
            walletdb.ErasePool(nIndex);
        }
        setExternalKeyPool.clear();

        for (int64_t nIndex : set_pre_split_keypool) {
            walletdb.ErasePool(nIndex);
        }
        set_pre_split_keypool.clear();

        m_pool_key_to_index.clear();

        if (!TopUpKeyPool()) {
            return false;
        }
        LogPrintf("CWallet::NewKeyPool rewrote keypool\n");
    }
    return true;
}

size_t CWallet::KeypoolCountExternalKeys()
{
    AssertLockHeld(cs_wallet); // setExternalKeyPool
    return setExternalKeyPool.size() + set_pre_split_keypool.size();
}

void CWallet::LoadKeyPool(int64_t nIndex, const CKeyPool &keypool)
{
    AssertLockHeld(cs_wallet);
    if (keypool.m_pre_split) {
        set_pre_split_keypool.insert(nIndex);
    } else if (keypool.fInternal) {
        setInternalKeyPool.insert(nIndex);
    } else {
        setExternalKeyPool.insert(nIndex);
    }
    m_max_keypool_index = std::max(m_max_keypool_index, nIndex);
    m_pool_key_to_index[keypool.vchPubKey.GetID()] = nIndex;

    // If no metadata exists yet, create a default with the pool key's
    // creation time. Note that this may be overwritten by actually
    // stored metadata for that key later, which is fine.
    CKeyID keyid = keypool.vchPubKey.GetID();
    if (mapKeyMetadata.count(keyid) == 0)
        mapKeyMetadata[keyid] = CKeyMetadata(keypool.nTime);
}

bool CWallet::TopUpKeyPool(unsigned int kpSize)
{
    if (IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        return false;
    }
    {
        LOCK(cs_wallet);

        if (IsLocked())
            return false;

        // Top up key pool
        unsigned int nTargetSize;
        if (kpSize > 0)
            nTargetSize = kpSize;
        else
            nTargetSize = std::max(GetArg("-keypool", 100), (int64_t) 0);

        // count amount of available keys (internal, external)
        // make sure the keypool of external keys fits the user selected target (-keypool)
        int64_t missingExternal = std::max(std::max((int64_t) nTargetSize, (int64_t) 1) - (int64_t)setExternalKeyPool.size(), (int64_t) 0);
        int64_t missingInternal = std::max(std::max((int64_t) nTargetSize, (int64_t) 1) - (int64_t)setInternalKeyPool.size(), (int64_t) 0);

        if (!IsHDEnabled() || !CanSupportFeature(FEATURE_HD_SPLIT))
        {
            // don't create extra internal keys
            missingInternal = 0;
        }
        bool internal = false;
        CWalletDB walletdb(strWalletFile);

        for (int64_t i = missingInternal + missingExternal; i--;) {
            if (i < missingInternal)
                internal = true;

            assert(m_max_keypool_index < std::numeric_limits<int64_t>::max()); // How in the hell did you use so many keys?
            int64_t index = ++m_max_keypool_index;

            CPubKey pubkey(GenerateNewKey(walletdb, internal));
            if (!walletdb.WritePool(index, CKeyPool(pubkey, internal)))
                throw std::runtime_error("TopUpKeyPool(): writing generated key failed");

            if (internal) {
                setInternalKeyPool.insert(index);
            } else {
                setExternalKeyPool.insert(index);
            }
            m_pool_key_to_index[pubkey.GetID()] = index;

            double dProgress = 100.f * index / (nTargetSize + 1);
            if (dProgress < 100.1) {
                std::string strMsg = strprintf(_("Loading wallet... (%3.2f %%)"), dProgress);
                uiInterface.InitMessage(strMsg);
            }
        }
        if (missingInternal + missingExternal > 0) {
            LogPrintf("keypool added %d keys (%d internal), size=%u (%u internal)\n", missingInternal + missingExternal, missingInternal, setInternalKeyPool.size() + setExternalKeyPool.size() + set_pre_split_keypool.size(), setInternalKeyPool.size());
        }
    }
    return true;
}

void CWallet::ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool, bool fRequestedInternal)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();
    {
        LOCK(cs_wallet);

        if (!IsLocked())
            TopUpKeyPool();

        bool fReturningInternal = IsHDEnabled() && CanSupportFeature(FEATURE_HD_SPLIT) && fRequestedInternal;
        std::set<int64_t>& setKeyPool = set_pre_split_keypool.empty() ? (fReturningInternal ? setInternalKeyPool : setExternalKeyPool) : set_pre_split_keypool;

        // Get the oldest key
        if (setKeyPool.empty())
            return;

        CWalletDB walletdb(strWalletFile);

        auto it = setKeyPool.begin();
        nIndex = *it;
        setKeyPool.erase(it);
        if (!walletdb.ReadPool(nIndex, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": read failed");
        }
        if (!HaveKey(keypool.vchPubKey.GetID())) {
            throw std::runtime_error(std::string(__func__) + ": unknown key in key pool");
        }
        if (set_pre_split_keypool.size() == 0 && keypool.fInternal != fReturningInternal) {
            throw std::runtime_error(std::string(__func__) + ": keypool entry misclassified");
        }

        assert(keypool.vchPubKey.IsValid());
        m_pool_key_to_index.erase(keypool.vchPubKey.GetID());
        LogPrintf("keypool reserve %d\n", nIndex);
    }
}

void CWallet::KeepKey(int64_t nIndex)
{
    // Remove from key pool
    if (fFileBacked) {
        CWalletDB walletdb(strWalletFile);
        walletdb.ErasePool(nIndex);
    }
    //LogPrintf("keypool keep %d\n", nIndex);
}

void CWallet::ReturnKey(int64_t nIndex, bool fInternal, const CPubKey& pubkey)
{
    // Return to key pool
    {
        LOCK(cs_wallet);
        if (fInternal) {
            setInternalKeyPool.insert(nIndex);
        } else if (!set_pre_split_keypool.empty()) {
            set_pre_split_keypool.insert(nIndex);
        } else {
            setExternalKeyPool.insert(nIndex);
        }
        m_pool_key_to_index[pubkey.GetID()] = nIndex;
    }
    LogPrintf("keypool return %d\n", nIndex);
}

bool CWallet::GetKeyFromPool(CPubKey& result, bool internal) {
    if (IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        return false;
    }
    CKeyPool keypool;{
        LOCK(cs_wallet);
        int64_t nIndex = 0;
        ReserveKeyFromKeyPool(nIndex, keypool, internal);
        if (nIndex == -1) {
            if (IsLocked()) return false;
            CWalletDB walletdb(strWalletFile);
            result = GenerateNewKey(walletdb, internal);
            return true;
        }
        KeepKey(nIndex);
        result = keypool.vchPubKey;
    }
    return true;
}

static int64_t GetOldestKeyTimeInPool(const std::set<int64_t>& setKeyPool, CWalletDB& walletdb) {
    if (setKeyPool.empty()) {
        return GetTime();
    }

    CKeyPool keypool;
    int64_t nIndex = *(setKeyPool.begin());
    if (!walletdb.ReadPool(nIndex, keypool)) {
        throw std::runtime_error(std::string(__func__) + ": read oldest key in keypool failed");
    }
    assert(keypool.vchPubKey.IsValid());
    return keypool.nTime;
}

int64_t CWallet::GetOldestKeyPoolTime() {

    CWalletDB walletdb(strWalletFile);
    // load oldest key from keypool, get time and return
    int64_t oldestKey = GetOldestKeyTimeInPool(setExternalKeyPool, walletdb);
    if (IsHDEnabled() && CanSupportFeature(FEATURE_HD_SPLIT)) {
        oldestKey = std::max(GetOldestKeyTimeInPool(setInternalKeyPool, walletdb), oldestKey);
        if (!set_pre_split_keypool.empty()) {
            oldestKey = std::max(GetOldestKeyTimeInPool(set_pre_split_keypool, walletdb), oldestKey);
        }
    }
    return oldestKey;
}

std::map<CTxDestination, CAmount> CWallet::GetAddressBalances() {
    map<CTxDestination, CAmount> balances;{
        LOCK(cs_wallet);
        for (PAIRTYPE(uint256, CWalletTx) walletEntry : mapWallet) {
            CWalletTx* pcoin = &walletEntry.second;

            if (!IsFinalTx(*pcoin->tx) || !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? 0 : 1))
                continue;

            for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
                CTxDestination addr;
                if (!IsMine(pcoin->tx->vout[i]))
                    continue;
                if (!ExtractDestination(pcoin->tx->vout[i].scriptPubKey, addr))
                    continue;

                CAmount n = IsSpent(walletEntry.first, i) ? 0 : pcoin->tx->vout[i].nValue;

                if (!balances.count(addr))
                    balances[addr] = 0;
                balances[addr] += n;
            }
        }
    }

    return balances;
}

std::set< std::set<CTxDestination> > CWallet::GetAddressGroupings()
{
    AssertLockHeld(cs_wallet); // mapWallet
    std::set< std::set<CTxDestination> > groupings;
    std::set<CTxDestination> grouping;

    for (const auto& walletEntry : mapWallet) {
        const CWalletTx *pcoin = &walletEntry.second;

        if (pcoin->tx->vin.size() > 0) {
            bool any_mine = false;
            // group all input addresses with each other
            for (CTxIn txin : pcoin->tx->vin) {
                CTxDestination address;
                if (!IsMine(txin)) /* If this input isn't mine, ignore it */
                    continue;
                if (!ExtractDestination(mapWallet[txin.prevout.hash].tx->vout[txin.prevout.n].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                any_mine = true;
            }

            // group change with input addresses
            if (any_mine) {
                for (CTxOut txout : pcoin->tx->vout)
                    if (IsChange(txout)) {
                        CTxDestination txoutAddr;
                        if (!ExtractDestination(txout.scriptPubKey, txoutAddr))
                            continue;
                        grouping.insert(txoutAddr);
                    }
            }
            if (grouping.size() > 0) {
                groupings.insert(grouping);
                grouping.clear();
            }
        }

        // group lone addrs by themselves
        for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++)
            if (IsMine(pcoin->tx->vout[i])) {
                CTxDestination address;
                if (!ExtractDestination(pcoin->tx->vout[i].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
    }

    std::set< std::set<CTxDestination>* > uniqueGroupings; // a set of pointers to groups of addresses
    std::map< CTxDestination, std::set<CTxDestination>* > setmap;  // map addresses to the unique group containing it
    for (std::set<CTxDestination> grouping : groupings) {
        // make a set of all the groups hit by this new group
        std::set< std::set<CTxDestination>* > hits;
        std::map< CTxDestination, std::set<CTxDestination>* >::iterator it;
        for (CTxDestination address : grouping)
            if ((it = setmap.find(address)) != setmap.end())
                hits.insert((*it).second);

        // merge all hit groups into a new single group and delete old groups
        std::set<CTxDestination>* merged = new std::set<CTxDestination>(grouping);
        for (std::set<CTxDestination>* hit : hits)
        {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }
        uniqueGroupings.insert(merged);

        // update setmap
        for (CTxDestination element : *merged)
            setmap[element] = merged;
    }

    std::set< std::set<CTxDestination> > ret;
    for (std::set<CTxDestination>* uniqueGrouping : uniqueGroupings) {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}

std::set<CTxDestination> CWallet::GetAccountAddresses(const std::string& strAccount) const
{
    LOCK(cs_wallet);
    std::set<CTxDestination> result;
    for (const std::pair<CTxDestination, CAddressBookData>& item : mapAddressBook)
    {
        const CTxDestination& address = item.first;
        const std::string& strName = item.second.name;
        if (strName == strAccount)
            result.insert(address);
    }
    return result;
}

// disable transaction (only for coinstake)
void CWallet::DisableTransaction(const CTransaction &tx)
{
    if (!tx.IsCoinStake() || !IsFromMe(tx))
        return; // only disconnecting coinstake requires marking input unspent

    LOCK(cs_wallet);
    uint256 hash = tx.GetHash();
    if(AbandonTransaction(hash))
    {
        RemoveFromSpends(hash);
        std::set<CWalletTx*> setCoins;
        for(const CTxIn& txin : tx.vin)
        {
            CWalletTx &coin = mapWallet[txin.prevout.hash];
            coin.BindWallet(this);
            NotifyTransactionChanged(this, coin.GetHash(), CT_UPDATED);
        }
        CWalletTx& wtx = mapWallet[hash];
        wtx.BindWallet(this);
        NotifyTransactionChanged(this, hash, CT_DELETED);
    }
}

bool CReserveKey::GetReservedKey(CPubKey& pubkey, bool internal) {
    if (nIndex == -1) {
        CKeyPool keypool;
        pwalletMain->ReserveKeyFromKeyPool(nIndex, keypool, internal);
        if (nIndex != -1)
            vchPubKey = keypool.vchPubKey;
        else {
            return false;
        }
        fInternal = keypool.fInternal;
    }
    assert(vchPubKey.IsValid());
    pubkey = vchPubKey;
    return true;
}

void CReserveKey::KeepKey()
{
    if (nIndex != -1)
        pwallet->KeepKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CReserveKey::ReturnKey()
{
    if (nIndex != -1) {
        pwallet->ReturnKey(nIndex, fInternal, vchPubKey);
    }
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CWallet::MarkReserveKeysAsUsed(int64_t keypool_id)
{
    AssertLockHeld(cs_wallet);
    bool internal = setInternalKeyPool.count(keypool_id);
    if (!internal) assert(setExternalKeyPool.count(keypool_id) || set_pre_split_keypool.count(keypool_id));
    std::set<int64_t> *setKeyPool = internal ? &setInternalKeyPool : (set_pre_split_keypool.empty() ? &setExternalKeyPool : &set_pre_split_keypool);
    auto it = setKeyPool->begin();

    CWalletDB walletdb(strWalletFile);
    while (it != std::end(*setKeyPool)) {
        const int64_t& index = *(it);
        if (index > keypool_id) break;

        CKeyPool keypool;
        if (walletdb.ReadPool(index, keypool)) {
            m_pool_key_to_index.erase(keypool.vchPubKey.GetID());
        }
        walletdb.ErasePool(index);
        LogPrintf("keypool index %d removed\n", index);
        it = setKeyPool->erase(it);
    }
}

void CWallet::GetScriptForMining(std::shared_ptr<CReserveScript> &script)
{
    std::shared_ptr<CReserveKey> rKey = std::make_shared<CReserveKey>(this);
    CPubKey pubkey;
    if (!rKey->GetReservedKey(pubkey))
        return;

    script = rKey;
    script->reserveScript = CScript() << ToByteVector(pubkey) << OP_CHECKSIG;
}

bool CWallet::UpdatedTransaction(const uint256& hashTx)
{
    {
        LOCK(cs_wallet);
        // Only notify UI if this transaction is in this wallet
        std::map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(hashTx);
        if (mi != mapWallet.end()) {
            NotifyTransactionChanged(this, hashTx, CT_UPDATED);
            return true;
        }
    }
    return false;
}

void CWallet::LockCoin(COutPoint& output)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.insert(output);
}

void CWallet::UnlockCoin(COutPoint& output)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.erase(output);
}

void CWallet::UnlockAllCoins()
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.clear();
}

bool CWallet::IsLockedCoin(uint256 hash, unsigned int n) const
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    COutPoint outpt(hash, n);

    return (setLockedCoins.count(outpt) > 0);
}

void CWallet::ListLockedCoins(std::vector<COutPoint>& vOutpts)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    for (std::set<COutPoint>::iterator it = setLockedCoins.begin();
         it != setLockedCoins.end(); it++) {
        COutPoint outpt = (*it);
        vOutpts.push_back(outpt);
    }
}

/** @} */ // end of Actions

void CWallet::GetKeyBirthTimes(std::map<CTxDestination, int64_t> &mapKeyBirth) const {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    mapKeyBirth.clear();

    // get birth times for keys with metadata
    for (const auto& entry : mapKeyMetadata) {
        if (entry.second.nCreateTime) {
            mapKeyBirth[entry.first] = entry.second.nCreateTime;
        }
    }

    // map in which we'll infer heights of other keys
    CBlockIndex *pindexMax = chainActive[std::max(0, chainActive.Height() - 144)]; // the tip can be reorganized; use a 144-block safety margin
    std::map<CKeyID, CBlockIndex*> mapKeyFirstBlock;
    std::set<CKeyID> setKeys = GetKeys();
    for (const CKeyID& keyid : setKeys) {
        if (mapKeyBirth.count(keyid) == 0)
            mapKeyFirstBlock[keyid] = pindexMax;
    }
    setKeys.clear();

    // if there are no such keys, we're done
    if (mapKeyFirstBlock.empty())
        return;

    // find first block that affects those keys, if there are any left
    std::vector<CKeyID> vAffected;
    for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); it++) {
        // iterate over all wallet transactions...
        const CWalletTx &wtx = (*it).second;
        BlockMap::const_iterator blit = mapBlockIndex.find(wtx.hashBlock);
        if (blit != mapBlockIndex.end() && chainActive.Contains(blit->second)) {
            // ... which are already in a block
            int nHeight = blit->second->nHeight;
            for (const CTxOut &txout : wtx.tx->vout) {
                // iterate over all their outputs
                CAffectedKeysVisitor(*this, vAffected).Process(txout.scriptPubKey);
                for (const CKeyID &keyid : vAffected) {
                    // ... and all their affected keys
                    std::map<CKeyID, CBlockIndex*>::iterator rit = mapKeyFirstBlock.find(keyid);
                    if (rit != mapKeyFirstBlock.end() && nHeight < rit->second->nHeight)
                        rit->second = blit->second;
                }
                vAffected.clear();
            }
        }
    }

    // Extract block timestamps for those keys
    for (std::map<CKeyID, CBlockIndex*>::const_iterator it = mapKeyFirstBlock.begin(); it != mapKeyFirstBlock.end(); it++)
        mapKeyBirth[it->first] = it->second->GetBlockTime() - TIMESTAMP_WINDOW; // block times can be 2h off
}

/**
 * Compute smart timestamp for a transaction being added to the wallet.
 *
 * Logic:
 * - If sending a transaction, assign its timestamp to the current time.
 * - If receiving a transaction outside a block, assign its timestamp to the
 *   current time.
 * - If receiving a block with a future timestamp, assign all its (not already
 *   known) transactions' timestamps to the current time.
 * - If receiving a block with a past timestamp, before the most recent known
 *   transaction (that we care about), assign all its (not already known)
 *   transactions' timestamps to the same timestamp as that most-recent-known
 *   transaction.
 * - If receiving a block with a past timestamp, but after the most recent known
 *   transaction, assign all its (not already known) transactions' timestamps to
 *   the block time.
 *
 * For more information see CWalletTx::nTimeSmart,
 * https://bitcointalk.org/?topic=54527, or
 * https://github.com/bitcoin/bitcoin/pull/1393.
 */
unsigned int CWallet::ComputeTimeSmart(const CWalletTx& wtx) const
{
    unsigned int nTimeSmart = wtx.nTimeReceived;
    if (!wtx.hashUnset()) {
        if (mapBlockIndex.count(wtx.hashBlock)) {
            int64_t latestNow = wtx.nTimeReceived;
            int64_t latestEntry = 0;

            // Tolerate times up to the last timestamp in the wallet not more than 5 minutes into the future
            int64_t latestTolerated = latestNow + 300;
            const TxItems& txOrdered = wtxOrdered;
            for (auto it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
                CWalletTx* const pwtx = it->second.first;
                if (pwtx == &wtx) {
                    continue;
                }
                CAccountingEntry* const pacentry = it->second.second;
                int64_t nSmartTime;
                if (pwtx) {
                    nSmartTime = pwtx->nTimeSmart;
                    if (!nSmartTime) {
                        nSmartTime = pwtx->nTimeReceived;
                    }
                } else {
                    nSmartTime = pacentry->nTime;
                }
                if (nSmartTime <= latestTolerated) {
                    latestEntry = nSmartTime;
                    if (nSmartTime > latestNow) {
                        latestNow = nSmartTime;
                    }
                    break;
                }
            }

            int64_t blocktime = mapBlockIndex[wtx.hashBlock]->GetBlockTime();
            nTimeSmart = std::max(latestEntry, std::min(blocktime, latestNow));
        } else {
            LogPrintf("%s: found %s in block %s not in index\n", __func__, wtx.GetHash().ToString(), wtx.hashBlock.ToString());
        }
    }
    return nTimeSmart;
}

bool CWallet::AddDestData(const CTxDestination& dest, const std::string& key, const std::string& value)
{
    if (boost::get<CNoDestination>(&dest))
        return false;

    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteDestData(EncodeDestination(dest), key, value);
}

bool CWallet::EraseDestData(const CTxDestination& dest, const std::string& key)
{
    if (!mapAddressBook[dest].destdata.erase(key))
        return false;
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).EraseDestData(EncodeDestination(dest), key);
}

bool CWallet::LoadDestData(const CTxDestination& dest, const std::string& key, const std::string& value)
{
    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    return true;
}

bool CWallet::GetDestData(const CTxDestination& dest, const std::string& key, std::string* value) const
{
    std::map<CTxDestination, CAddressBookData>::const_iterator i = mapAddressBook.find(dest);
    if (i != mapAddressBook.end()) {
        CAddressBookData::StringMap::const_iterator j = i->second.destdata.find(key);
        if (j != i->second.destdata.end()) {
            if (value)
                *value = j->second;
            return true;
        }
    }
    return false;
}

std::vector<std::string> CWallet::GetDestValues(const std::string& prefix) const
{
    LOCK(cs_wallet);
    std::vector<std::string> values;
    for (const auto& address : mapAddressBook) {
        for (const auto& data : address.second.destdata) {
            if (!data.first.compare(0, prefix.size(), prefix)) {
                values.emplace_back(data.second);
            }
        }
    }
    return values;
}

void CWallet::MarkPreSplitKeys()
{
    CWalletDB walletdb(strWalletFile);
    for (auto it = setExternalKeyPool.begin(); it != setExternalKeyPool.end();) {
        int64_t index = *it;
        CKeyPool keypool;
        if (!walletdb.ReadPool(index, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": read keypool entry failed");
        }
        keypool.m_pre_split = true;
        if (!walletdb.WritePool(index, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": writing modified keypool entry failed");
        }
        set_pre_split_keypool.insert(index);
        it = setExternalKeyPool.erase(it);
    }
}

void CWallet::AutoCombineDust()
{
    if (IsInitialBlockDownload() || IsLocked()) {
        return;
    }

    map<CTxDestination, vector<COutput> > mapCoinsByAddress = AvailableCoinsByAddress(true, 0);

    //coins are sectioned by address. This combination code only wants to combine inputs that belong to the same address
    for (map<CTxDestination, vector<COutput> >::iterator it = mapCoinsByAddress.begin(); it != mapCoinsByAddress.end(); it++) {
        vector<COutput> vCoins, vRewardCoins;
        vCoins = it->second;

        //find masternode rewards that need to be combined
        CCoinControl* coinControl = new CCoinControl();
        CAmount nTotalRewardsValue = 0;
        for (const COutput& out : vCoins) {

            if (!out.fSpendable)
                continue;

            //no coins should get this far if they dont have proper maturity, this is double checking
            if (out.tx->IsCoinStake() && out.tx->GetDepthInMainChain() < Params().COINBASE_MATURITY() + 1)
                continue;

            if (out.Value() > nAutoCombineThreshold * COIN)
                continue;

            COutPoint outpt(out.tx->GetHash(), out.i);
            coinControl->Select(outpt);
            vRewardCoins.push_back(out);
            nTotalRewardsValue += out.Value();
        }

        //if no inputs found then return
        if (!coinControl->HasSelected())
            continue;

        //we cannot combine one coin with itself
        if (vRewardCoins.size() <= 1)
            continue;

        vector<pair<CScript, int64_t> > vecSend;
        CScript scriptPubKey = GetScriptForDestination(it->first);
        vecSend.push_back(std::make_pair(scriptPubKey, nTotalRewardsValue));

         //Send change to same address
        CTxDestination destMyAddress;
        if (!ExtractDestination(scriptPubKey, destMyAddress))
        {
            LogPrintf("AutoCombineDust: failed to extract destination\n");
            continue;
        }
        coinControl->destChange = destMyAddress;

        // Create the transaction and commit it to the network
        CWalletTx wtx;
        CReserveKey keyChange(this); // this change address does not end up being used, because change is returned with coin control switch
        string strErr;
        int64_t nFeeRet = 0;
#if 1
        //get the fee amount
        CWalletTx wtxdummy;
        CreateTransaction(vecSend, wtxdummy, keyChange, nFeeRet, strErr, coinControl, ALL_COINS, false, CAmount(0));

        vecSend[0].second = nTotalRewardsValue - nFeeRet - 500;

        if (!CreateTransaction(vecSend, wtx, keyChange, nFeeRet, strErr, coinControl, ALL_COINS, false, CAmount(0))) {
            LogPrintf("AutoCombineDust createtransaction failed, reason: %s\n", strErr);
            continue;
        }
#endif

#if 0
       // Added 5% fee tp prevent "Insufficient funds" errors
        vecSend[0].second = nTotalRewardsValue - (nTotalRewardsValue / 5);

        if (!CreateTransaction(vecSend, wtx, keyChange, nFeeRet, strErr, coinControl, ALL_COINS, false, CAmount(0))) {
            LogPrintf("AutoCombineDust createtransaction failed, reason: %s\n", strErr);
            continue;
        }
#endif

#if 0
        //we don't combine below the threshold unless the fees are 0 to avoid paying fees over fees over fees
        if (nTotalRewardsValue < nAutoCombineThreshold * COIN && nFeeRet > 0)
            continue;
#else
        if (!CommitTransaction(wtx, keyChange)) {
            LogPrintf("AutoCombineDust transaction commit failed\n");
            continue;
        }
#endif
        LogPrintf("AutoCombineDust sent transaction\n");

        delete coinControl;
    }
}

bool CWallet::MultiSend()
{
    if (IsInitialBlockDownload() || IsLocked()) {
        return false;
    }

    if (chainActive.Height() <= nLastMultiSendHeight) {
        LogPrintf("Multisend: lastmultisendheight is higher than current best height\n");
        return false;
    }

    std::vector<COutput> vCoins;
    AvailableCoins(vCoins);
    int stakeSent = 0;
    int mnSent = 0;
    for (const COutput& out : vCoins) {
        //need output with precise confirm count - this is how we identify which is the output to send
        if (out.tx->GetDepthInMainChain() != Params().COINBASE_MATURITY() + 1)
            continue;

        COutPoint outpoint(out.tx->GetHash(), out.i);
        bool sendMSonMNReward = fMultiSendMasternodeReward && outpoint.IsMasternodeReward(*out.tx->tx);
        bool sendMSOnStake = fMultiSendStake && out.tx->IsCoinStake() && !sendMSonMNReward; //output is either mnreward or stake reward, not both

        if (!(sendMSOnStake || sendMSonMNReward))
            continue;

        CTxDestination destMyAddress;
        if (!ExtractDestination(out.tx->tx->vout[out.i].scriptPubKey, destMyAddress)) {
            LogPrintf("Multisend: failed to extract destination\n");
            continue;
        }

        //Disabled Addresses won't send MultiSend transactions
        if (vDisabledAddresses.size() > 0) {
            for (unsigned int i = 0; i < vDisabledAddresses.size(); i++) {
                if (vDisabledAddresses[i] == EncodeDestination(destMyAddress)) {
                    LogPrintf("Multisend: disabled address preventing multisend\n");
                    return false;
                }
            }
        }

        // create new coin control, populate it with the selected utxo, create sending vector
        CCoinControl* cControl = new CCoinControl();
        COutPoint outpt(out.tx->GetHash(), out.i);
        cControl->Select(outpt);
        cControl->destChange = destMyAddress;

        CWalletTx wtx;
        CReserveKey keyChange(this); // this change address does not end up being used, because change is returned with coin control switch
        int64_t nFeeRet = 0;
        vector<pair<CScript, int64_t> > vecSend;

        // loop through multisend vector and add amounts and addresses to the sending vector
        const isminefilter filter = ISMINE_SPENDABLE;
        int64_t nAmount = 0;
        for (unsigned int i = 0; i < vMultiSend.size(); i++) {
            // MultiSend vector is a pair of 1)Address as a std::string 2) Percent of stake to send as an int
            nAmount = ((out.tx->GetCredit(filter) - out.tx->GetDebit(filter)) * vMultiSend[i].second) / 100;
            CTxDestination strAddSend = DecodeDestination(vMultiSend[i].first);
            CScript scriptPubKey;
            scriptPubKey = GetScriptForDestination(strAddSend);
            vecSend.push_back(std::make_pair(scriptPubKey, nAmount));
        }

        //get the fee amount
        CWalletTx wtxdummy;
        string strErr;
        CreateTransaction(vecSend, wtxdummy, keyChange, nFeeRet, strErr, cControl, ALL_COINS, false, CAmount(0));
        CAmount nLastSendAmount = vecSend[vecSend.size() - 1].second;
        if (nLastSendAmount < nFeeRet + 500) {
            LogPrintf("%s: fee of %s is too large to insert into last output\n");
            return false;
        }
        vecSend[vecSend.size() - 1].second = nLastSendAmount - nFeeRet - 500;

        // Create the transaction and commit it to the network
        if (!CreateTransaction(vecSend, wtx, keyChange, nFeeRet, strErr, cControl, ALL_COINS, false, CAmount(0))) {
            LogPrintf("MultiSend createtransaction failed\n");
            return false;
        }

        if (!CommitTransaction(wtx, keyChange)) {
            LogPrintf("MultiSend transaction commit failed\n");
            return false;
        } else
            fMultiSendNotify = true;

        delete cControl;

        //write nLastMultiSendHeight to DB
        CWalletDB walletdb(strWalletFile);
        nLastMultiSendHeight = chainActive.Height();
        if (!walletdb.WriteMSettings(fMultiSendStake, fMultiSendMasternodeReward, nLastMultiSendHeight))
            LogPrintf("Failed to write MultiSend setting to DB\n");

        LogPrintf("MultiSend successfully sent\n");
        if (sendMSOnStake)
            stakeSent++;
        else
            mnSent++;

        //stop iterating if we are done
        if (mnSent > 0 && stakeSent > 0)
            return true;
        if (stakeSent > 0 && !fMultiSendMasternodeReward)
            return true;
        if (mnSent > 0 && !fMultiSendStake)
            return true;
    }
    bZeroBalanceAddressToken = GetBoolArg("-zerobalanceaddresstoken", DEFAULT_SPEND_ZEROCONF_CHANGE);
    return true;
}

bool CWallet::LoadToken(const CTokenInfo &token) {
    uint256 hash = token.GetHash();
    mapToken[hash] = token;

    return true;
}

bool CWallet::LoadTokenTx(const CTokenTx &tokenTx) {
    uint256 hash = tokenTx.GetHash();
    mapTokenTx[hash] = tokenTx;

    return true;
}

bool CWallet::AddTokenEntry(const CTokenInfo &token, bool fFlushOnClose) {
    LOCK(cs_wallet);

    CWalletDB walletdb(strWalletFile, "r+", fFlushOnClose);

    uint256 hash = token.GetHash();

    bool fInsertedNew = true;

    std::map<uint256, CTokenInfo>::iterator it = mapToken.find(hash);
    if(it!=mapToken.end()) {
        fInsertedNew = false;
    }

    // Write to disk
    CTokenInfo wtoken = token;
    if(!fInsertedNew) {
        wtoken.nCreateTime = GetAdjustedTime();
    } else {
        wtoken.nCreateTime = it->second.nCreateTime;
    }

    if (!walletdb.WriteToken(wtoken))
        return false;

    mapToken[hash] = wtoken;

    NotifyTokenChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

    // Refresh token tx
    if(fInsertedNew)
    {
        for(auto it = mapTokenTx.begin(); it != mapTokenTx.end(); it++)
        {
            uint256 tokenTxHash = it->second.GetHash();
            NotifyTokenTransactionChanged(this, tokenTxHash, CT_UPDATED);
        }
    }

    LogPrintf("AddTokenEntry %s\n", wtoken.GetHash().ToString());

    return true;
}

bool CWallet::AddTokenTxEntry(const CTokenTx &tokenTx, bool fFlushOnClose) {
    LOCK(cs_wallet);

    CWalletDB walletdb(strWalletFile, "r+", fFlushOnClose);

    uint256 hash = tokenTx.GetHash();

    bool fInsertedNew = true;

    std::map<uint256, CTokenTx>::iterator it = mapTokenTx.find(hash);
    if(it!=mapTokenTx.end()) {
        fInsertedNew = false;
    }

    // Write to disk
    CTokenTx wtokenTx = tokenTx;
    if(!fInsertedNew) {
        wtokenTx.strLabel = it->second.strLabel;
    }
    const CBlockIndex *pIndex = chainActive[wtokenTx.blockNumber];
    wtokenTx.nCreateTime = pIndex ? pIndex->GetBlockTime() : GetAdjustedTime();

    if (!walletdb.WriteTokenTx(wtokenTx))
        return false;

    mapTokenTx[hash] = wtokenTx;

    NotifyTokenTransactionChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

    LogPrintf("AddTokenTxEntry %s\n", wtokenTx.GetHash().ToString());

    return true;
}

CKeyPool::CKeyPool() {
    nTime = GetTime();
    fInternal = false;
    m_pre_split = false;
}

CKeyPool::CKeyPool(const CPubKey& vchPubKeyIn, bool internalIn) {
    nTime = GetTime();
    vchPubKey = vchPubKeyIn;
    fInternal = internalIn;
    m_pre_split = false;
}

CWalletKey::CWalletKey(int64_t nExpires) {
    nTimeCreated = (nExpires ? GetTime() : 0);
    nTimeExpires = nExpires;
}

void CMerkleTx::SetMerkleBranch(const CBlockIndex* pindex, int posInBlock)
{
    // Update the tx's hashBlock
    hashBlock = pindex->GetBlockHash();

    // set the position of the transaction in the block
    nIndex = posInBlock;
}

int CMerkleTx::GetDepthInMainChainINTERNAL(const CBlockIndex*& pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;
    AssertLockHeld(cs_main);

    // Find the block it claims to be in
    CBlockIndex* pindex = LookupBlockIndex(hashBlock);
    if (!pindex || !chainActive.Contains(pindex))
        return 0;
#if 0
    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified) {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }
#endif
    pindexRet = pindex;
    return chainActive.Height() - pindex->nHeight + 1;
}

int CMerkleTx::GetDepthInMainChain(const CBlockIndex*& pindexRet, bool enableIX) const
{
    if (hashUnset())
        return 0;

    AssertLockHeld(cs_main);

    // Find the block it claims to be in
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !chainActive.Contains(pindex))
        return 0;

    pindexRet = pindex;
#if 0
    if (enableIX) {
            int signatures = GetTransactionLockSignatures();
            if (signatures >= INSTANTX_SIGNATURES_REQUIRED) {
                return nInstanTXDepth + nResult;
            }
    }
#endif
    return ((nIndex == -1) ? (-1) : 1) * (chainActive.Height() - pindex->nHeight + 1);
}

int CMerkleTx::GetBlocksToMaturity() const
{
    if (!IsCoinGenerated())
        return 0;
    return max(0, (Params().COINBASE_MATURITY() + 1) - GetDepthInMainChain());
}


bool CMerkleTx::AcceptToMemoryPool(bool fLimitFree, bool fRejectInsaneFee, bool ignoreFees)
{
    CValidationState state;
    return ::AcceptToMemoryPool(mempool, state, tx, fLimitFree, nullptr, fRejectInsaneFee, ignoreFees);
}

int CMerkleTx::GetTransactionLockSignatures() const
{
    if (fLargeWorkForkFound || fLargeWorkInvalidChainFound) return -2;
    if (!IsSporkActive(SPORK_7_INSTANTX)) return -3;
    if (!fEnableInstanTX) return -1;

    //compile consessus vote
    std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(GetHash());
    if (i != mapTxLocks.end()) {
        return (*i).second.CountSignatures();
    }

    return -1;
}

bool CMerkleTx::IsTransactionLockTimedOut() const
{
    if (!fEnableInstanTX) return 0;

    //compile consessus vote
    std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(GetHash());
    if (i != mapTxLocks.end()) {
        return GetTime() > (*i).second.nTimeout;
    }

    return false;
}

bool CWallet::AddLuxNodeConfig(CLuxNodeConfig nodeConfig)
{
    bool rv = CWalletDB(strWalletFile).WriteLuxNodeConfig(nodeConfig.sAlias, nodeConfig);
    if(rv)
	uiInterface.NotifyLuxNodeChanged(nodeConfig);

    return rv;
}

static const std::string OUTPUT_TYPE_STRING_LEGACY = "legacy";
static const std::string OUTPUT_TYPE_STRING_P2SH_SEGWIT = "p2sh-segwit";
static const std::string OUTPUT_TYPE_STRING_BECH32 = "bech32";

OutputType ParseOutputType(const std::string& type, OutputType default_type)
{
    if (type.empty()) {
        return default_type;
    } else if (type == OUTPUT_TYPE_STRING_LEGACY) {
        return OUTPUT_TYPE_LEGACY;
    } else if (type == OUTPUT_TYPE_STRING_P2SH_SEGWIT) {
        return OUTPUT_TYPE_P2SH_SEGWIT;
    } else if (type == OUTPUT_TYPE_STRING_BECH32) {
        return OUTPUT_TYPE_BECH32;
    } else {
        return OUTPUT_TYPE_NONE;
    }
}

const std::string& FormatOutputType(OutputType type)
{
    switch (type) {
    case OUTPUT_TYPE_LEGACY: return OUTPUT_TYPE_STRING_LEGACY;
    case OUTPUT_TYPE_P2SH_SEGWIT: return OUTPUT_TYPE_STRING_P2SH_SEGWIT;
    case OUTPUT_TYPE_BECH32: return OUTPUT_TYPE_STRING_BECH32;
    default: assert(false);
    }
}

void CWallet::LearnRelatedScripts(const CPubKey& key, OutputType type)
{
    if (key.IsCompressed() && (type == OUTPUT_TYPE_P2SH_SEGWIT || type == OUTPUT_TYPE_BECH32)) {
        CTxDestination witdest = WitnessV0KeyHash(key.GetID());
        CScript witprog = GetScriptForDestination(witdest);
        // Make sure the resulting program is solvable.
        assert(IsSolvable(*this, witprog));
        AddCScript(witprog);
    }
}

void CWallet::LearnAllRelatedScripts(const CPubKey& key)
{
    // OUTPUT_TYPE_P2SH_SEGWIT always adds all necessary scripts for all types.
    LearnRelatedScripts(key, OUTPUT_TYPE_P2SH_SEGWIT);
}

CTxDestination GetDestinationForKey(const CPubKey& key, OutputType type)
{
    switch (type) {
    case OUTPUT_TYPE_LEGACY: return key.GetID();
    case OUTPUT_TYPE_P2SH_SEGWIT:
    case OUTPUT_TYPE_BECH32: {
        if (!key.IsCompressed()) return key.GetID();
        CTxDestination witdest = WitnessV0KeyHash(key.GetID());
        CScript witprog = GetScriptForDestination(witdest);
        if (type == OUTPUT_TYPE_P2SH_SEGWIT) {
            return CScriptID(witprog);
        } else {
            return witdest;
        }
    }
    default: assert(false);
    }
}

std::vector<CTxDestination> GetAllDestinationsForKey(const CPubKey& key)
{
    CKeyID keyid = key.GetID();
    if (key.IsCompressed()) {
        CTxDestination segwit = WitnessV0KeyHash(keyid);
        CTxDestination p2sh = CScriptID(GetScriptForDestination(segwit));
        return std::vector<CTxDestination>{std::move(keyid), std::move(p2sh), std::move(segwit)};
    } else {
        return std::vector<CTxDestination>{std::move(keyid)};
    }
}

CTxDestination CWallet::AddAndGetDestinationForScript(const CScript& script, OutputType type)
{
    // Note that scripts over 520 bytes are not yet supported.
    switch (type) {
    case OUTPUT_TYPE_LEGACY:
        return CScriptID(script);
    case OUTPUT_TYPE_P2SH_SEGWIT:
    case OUTPUT_TYPE_BECH32: {
        WitnessV0ScriptHash hash;
        CSHA256().Write(script.data(), script.size()).Finalize(hash.begin());
        CTxDestination witdest = hash;
        CScript witprog = GetScriptForDestination(witdest);
        // Check if the resulting program is solvable (i.e. doesn't use an uncompressed key)
        if (!IsSolvable(*this, witprog)) return CScriptID(script);
        // Add the redeemscript, so that P2WSH and P2SH-P2WSH outputs are recognized as ours.
        AddCScript(witprog);
        if (type == OUTPUT_TYPE_BECH32) {
            return witdest;
        } else {
            return CScriptID(witprog);
        }
    }
    default: assert(false);
    }
}
bool CWallet::SetContractBook(const string &strAddress, const string &strName, const string &strAbi)
{
    bool fUpdated = false;
    {
        LOCK(cs_wallet); // mapContractBook
        auto mi = mapContractBook.find(strAddress);
        fUpdated = mi != mapContractBook.end();
        mapContractBook[strAddress].name = strName;
        mapContractBook[strAddress].abi = strAbi;
    }

    NotifyContractBookChanged(this, strAddress, strName, strAbi, (fUpdated ? CT_UPDATED : CT_NEW) );

    CWalletDB walletdb(strWalletFile, "r+");
    bool ret = walletdb.WriteContractData(strAddress, "name", strName);
    ret &= walletdb.WriteContractData(strAddress, "abi", strAbi);
    return ret;
}

bool CWallet::DelContractBook(const string &strAddress)
{
    {
        LOCK(cs_wallet); // mapContractBook
        mapContractBook.erase(strAddress);
    }

    NotifyContractBookChanged(this, strAddress, "", "", CT_DELETED);

    CWalletDB walletdb(strWalletFile, "r+");
    bool ret = walletdb.EraseContractData(strAddress, "name");
    ret &= walletdb.EraseContractData(strAddress, "abi");
    return ret;
}

bool CWallet::LoadContractData(const string &address, const string &key, const string &value)
{
    bool ret = true;
    if(key == "name")
    {
        mapContractBook[address].name = value;
    }
    else if(key == "abi")
    {
        mapContractBook[address].abi = value;
    }
    else
    {
        ret = false;
    }
    return ret;
}

uint256 CTokenInfo::GetHash() const
{
    return SerializeHash(*this, SER_GETHASH, 0);
}


uint256 CTokenTx::GetHash() const
{
    return SerializeHash(*this, SER_GETHASH, 0);
}


bool CWallet::GetTokenTxDetails(const CTokenTx &wtx, uint256 &credit, uint256 &debit, string &tokenSymbol, uint8_t &decimals) const
{
    LOCK(cs_wallet);
    bool ret = false;

    for(auto it = mapToken.begin(); it != mapToken.end(); it++)
    {
        CTokenInfo info = it->second;
        if(wtx.strContractAddress == info.strContractAddress)
        {
            if(wtx.strSenderAddress == info.strSenderAddress)
            {
                debit = wtx.nValue;
                tokenSymbol = info.strTokenSymbol;
                decimals = info.nDecimals;
                ret = true;
            }

            if(wtx.strReceiverAddress == info.strSenderAddress)
            {
                credit = wtx.nValue;
                tokenSymbol = info.strTokenSymbol;
                decimals = info.nDecimals;
                ret = true;
            }
        }
    }

    return ret;
}

bool CWallet::IsTokenTxMine(const CTokenTx &wtx) const
{
    LOCK(cs_wallet);
    bool ret = false;

    for(auto it = mapToken.begin(); it != mapToken.end(); it++)
    {
        CTokenInfo info = it->second;
        if(wtx.strContractAddress == info.strContractAddress)
        {
            if(wtx.strSenderAddress == info.strSenderAddress ||
               wtx.strReceiverAddress == info.strSenderAddress)
            {
                ret = true;
            }
        }
    }

    return ret;
}

bool CWallet::RemoveTokenEntry(const uint256 &tokenHash, bool fFlushOnClose)
{
    LOCK(cs_wallet);

    CWalletDB walletdb(strWalletFile, "r+", fFlushOnClose);

    bool fFound = false;

    std::map<uint256, CTokenInfo>::iterator it = mapToken.find(tokenHash);
    if(it!=mapToken.end())
    {
        fFound = true;
    }

    if(fFound)
    {
        // Remove from disk
        if (!walletdb.EraseToken(tokenHash))
            return false;

        mapToken.erase(it);

        NotifyTokenChanged(this, tokenHash, CT_DELETED);

        // Refresh token tx
        for(auto it = mapTokenTx.begin(); it != mapTokenTx.end(); it++)
        {
            uint256 tokenTxHash = it->second.GetHash();
            NotifyTokenTransactionChanged(this, tokenTxHash, CT_UPDATED);
        }
    }

    LogPrintf("RemoveTokenEntry %s\n", tokenHash.ToString());

    return true;
}
