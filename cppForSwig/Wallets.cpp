////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "Wallets.h"
#include "BlockDataManagerConfig.h"

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AssetWallet
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

const string AssetWallet::mainWalletDbName_ = string("MainWallet");

shared_ptr<AssetWallet_Single> AssetWallet_Single::
createFromPrivateRoot_Armory135(
   AddressEntryType defaultAddressType,
   SecureBinaryData&& privateRoot,
   unsigned lookup)
{
   //compute wallet ID
   auto&& pubkey = CryptoECDSA().ComputePublicKey(privateRoot);
   auto&& walletID = BtcUtils::getWalletID(pubkey);
   
   //set parentID to walletID if it's empty
   auto parentID = walletID;

   //create wallet file
   stringstream pathSS;
   string walletIDStr(walletID.getCharPtr(), walletID.getSize());
   pathSS << "armory_" << walletIDStr << "_wallet.lmdb";

   auto dbenv = getEnvFromFile(pathSS.str());
   auto walletPtr = make_shared<AssetWallet_Single>(dbenv);

   auto cypher = make_unique<Cypher_AES>();

   initWalletDb(
      walletPtr, 
      move(cypher),
      walletID, 
      defaultAddressType, 
      move(privateRoot), lookup);

   return walletPtr;
}

////////////////////////////////////////////////////////////////////////////////
void AssetWallet_Single::initWalletDb(
   shared_ptr<AssetWallet_Single> walletPtr,
   unique_ptr<Cypher> cypher,
   const BinaryData& parentID,
   AddressEntryType addressType,
   SecureBinaryData&& privateRoot,
   unsigned lookup)
{
   //compute wallet ID
   auto&& pubkey = CryptoECDSA().ComputePublicKey(privateRoot);
   auto&& walletID = BtcUtils::getWalletID(pubkey);

   //chaincode
   auto&& chaincode = BtcUtils::computeChainCode_Armory135(privateRoot);
   auto derScheme = make_shared<DerivationScheme_ArmoryLegacy>(move(chaincode));

   //create root AssetEntry
   auto rootAssetEntry = make_shared<AssetEntry_Single>(-1,
      move(pubkey), move(privateRoot), move(cypher));

   /**insert the original entries**/
   LMDBEnv::Transaction tx(walletPtr->dbEnv_, LMDB::ReadWrite);
   walletPtr->putHeaderData(
      parentID, walletID, derScheme, addressType, 0);

   {
      //root asset
      BinaryWriter bwKey;
      bwKey.put_uint32_t(ROOTASSET_KEY);

      auto&& data = rootAssetEntry->serialize();

      walletPtr->putData(bwKey.getData(), data);
   }
   
   //init walletptr from file
   walletPtr->readFromFile();

   {
      //asset lookup
      auto topEntryPtr = rootAssetEntry;

      if (lookup == UINT32_MAX)
         lookup = DERIVATION_LOOKUP;

      walletPtr->extendChain(rootAssetEntry, lookup);
   }

}

////////////////////////////////////////////////////////////////////////////////
void AssetWallet_Single::putHeaderData(const BinaryData& parentID,
   const BinaryData& walletID,
   shared_ptr<DerivationScheme> derScheme,
   AddressEntryType aet, int topUsedIndex)
{
   LMDBEnv::Transaction tx(dbEnv_, LMDB::ReadWrite);

   {
      //wallet type
      BinaryWriter bwKey;
      bwKey.put_uint32_t(WALLETTYPE_KEY);

      BinaryWriter bwData;
      bwData.put_var_int(1);
      bwData.put_uint8_t(WALLETTYPE_SINGLE);

      putData(bwKey, bwData);
   }

   AssetWallet::putHeaderData(parentID, walletID, derScheme, aet, topUsedIndex);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetWallet_Multisig> AssetWallet_Multisig::createFromPrivateRoot(
   AddressEntryType aet,
   unsigned M, unsigned N,
   SecureBinaryData&& privateRoot,
   unsigned lookup)
{
   if (aet != AddressEntryType_P2SH && aet != AddressEntryType_P2WSH)
      throw WalletException("invalid AddressEntryType for MS wallet");

   //compute wallet ID as hmac256(root pubkey, "M_of_N")
   stringstream mofn;
   mofn << M << "_of_" << N;
   auto&& pubkey = CryptoECDSA().ComputePublicKey(privateRoot);
   auto&& longID = BtcUtils::getHMAC256(pubkey, SecureBinaryData(mofn.str()));
   auto&& walletID = BtcUtils::getWalletID(longID);
   auto parentID = walletID;

   //create wallet file
   stringstream pathSS;
   string walletIDStr(walletID.getCharPtr(), walletID.getSize());
   pathSS << "armory_" << walletIDStr << "_wallet.lmdb";

   auto dbenv = getEnvFromFile(pathSS.str(), N + 1);
   auto walletPtr = make_shared<AssetWallet_Multisig>(dbenv);

   //create N sub wallets
   map<BinaryData, shared_ptr<AssetWallet_Single>> subWallets;

   for (unsigned i = 0; i < N; i++)
   {
      //get sub wallet root
      stringstream hmacMsg;
      hmacMsg << "Subwallet-" << i;

      SecureBinaryData subRoot(32);
      BtcUtils::getHMAC256(privateRoot.getPtr(), privateRoot.getSize(),
         hmacMsg.str().c_str(), hmacMsg.str().size(), subRoot.getPtr());

      auto subWalletPtr = make_shared<AssetWallet_Single>(dbenv, hmacMsg.str());
      auto cypher = make_unique<Cypher_AES>();

      AssetWallet_Single::initWalletDb(subWalletPtr, move(cypher),
         walletID, AddressEntryType_P2PKH, move(subRoot), lookup);

      subWallets[subWalletPtr->getID()] = subWalletPtr;
   }

   //create derScheme
   auto derScheme = make_shared<DerivationScheme_Multisig>(
      subWallets, N, M);

   {
      LMDBEnv::Transaction tx(walletPtr->dbEnv_, LMDB::ReadWrite);

      {
         //wallet type
         BinaryWriter bwKey;
         bwKey.put_uint32_t(WALLETTYPE_KEY);

         BinaryWriter bwData;
         bwData.put_var_int(1);
         bwData.put_uint8_t(WALLETTYPE_MULTISIG);

         walletPtr->putData(bwKey, bwData);
      }

      //header
      walletPtr->putHeaderData(
         walletID, walletID, derScheme, aet, 0);

      {
         //chainlength
         BinaryWriter bwKey;
         bwKey.put_uint8_t(ASSETENTRY_PREFIX);

         BinaryWriter bwData;
         bwData.put_var_int(4);
         bwData.put_uint32_t(lookup);

         walletPtr->putData(bwKey, bwData);
      }
   }

   //clean subwallets ptr and derScheme
   derScheme.reset();
   subWallets.clear();

   //load from db
   walletPtr->readFromFile();

   return walletPtr;
}

////////////////////////////////////////////////////////////////////////////////
void AssetWallet::putHeaderData(const BinaryData& parentID,
   const BinaryData& walletID, 
   shared_ptr<DerivationScheme> derScheme,
   AddressEntryType aet, int topUsedIndex)
{
   LMDBEnv::Transaction tx(dbEnv_, LMDB::ReadWrite);

   {
      //parent ID
      BinaryWriter bwKey;
      bwKey.put_uint32_t(PARENTID_KEY);

      BinaryWriter bwData;
      bwData.put_var_int(parentID.getSize());
      bwData.put_BinaryData(parentID);

      putData(bwKey, bwData);
   }

   {
      //wallet ID
      BinaryWriter bwKey;
      bwKey.put_uint32_t(WALLETID_KEY);

      BinaryWriter bwData;
      bwData.put_var_int(walletID.getSize());
      bwData.put_BinaryData(walletID);

      putData(bwKey, bwData);
   }

   {
      //derivation scheme
      BinaryWriter bwKey;
      bwKey.put_uint32_t(DERIVATIONSCHEME_KEY);

      auto&& data = derScheme->serialize();
      putData(bwKey.getData(), data);
   }

   {
      //default AddressEntryType
      BinaryWriter bwKey;
      bwKey.put_uint32_t(ADDRESSENTRYTYPE_KEY);

      BinaryWriter bwData;
      bwData.put_var_int(1);
      bwData.put_uint8_t(aet);

      putData(bwKey, bwData);
   }

   {
      //top used index
      BinaryWriter bwKey;
      bwKey.put_uint32_t(TOPUSEDINDEX_KEY);

      BinaryWriter bwData;
      bwData.put_var_int(4);
      bwData.put_int32_t(topUsedIndex);

      putData(bwKey, bwData);
   }
}

////////////////////////////////////////////////////////////////////////////////
BinaryDataRef AssetWallet::getDataRefForKey(const BinaryData& key) const
{
   /** The reference lifetime is tied to the db tx lifetime. The caller has to
   maintain the tx for as long as the data ref needs to be valid **/

   CharacterArrayRef keyRef(key.getSize(), key.getPtr());
   auto ref = db_->get_NoCopy(keyRef);

   if (ref.data == nullptr)
      throw NoEntryInWalletException();

   BinaryRefReader brr((const uint8_t*)ref.data, ref.len);
   auto len = brr.get_var_int();
   if (len != brr.getSizeRemaining())
      throw WalletException("on disk data length mismatch");

   return brr.get_BinaryDataRef(brr.getSizeRemaining());
}

////////////////////////////////////////////////////////////////////////////////
void AssetWallet_Single::readFromFile()
{
   //sanity check
   if (dbEnv_ == nullptr || db_ == nullptr)
      throw WalletException("uninitialized wallet object");

   LMDBEnv::Transaction tx(dbEnv_, LMDB::ReadOnly);

   {
      //parentId
      BinaryWriter bwKey;
      bwKey.put_uint32_t(PARENTID_KEY);

      auto parentIdRef = getDataRefForKey(bwKey.getData());
      parentID_ = parentIdRef;
   }

   {
      //walletId
      BinaryWriter bwKey;
      bwKey.put_uint32_t(WALLETID_KEY);
      auto walletIdRef = getDataRefForKey(bwKey.getData());

      walletID_ = walletIdRef;
   }

   {
      //derivation scheme
      BinaryWriter bwKey;
      bwKey.put_uint32_t(DERIVATIONSCHEME_KEY);
      auto derSchemeRef = getDataRefForKey(bwKey.getData());

      derScheme_ = DerivationScheme::deserialize(derSchemeRef);
   }

   {
      //default AddressEntryType
      BinaryWriter bwKey;
      bwKey.put_uint32_t(ADDRESSENTRYTYPE_KEY);
      auto defaultAetRef = getDataRefForKey(bwKey.getData());

      if (defaultAetRef.getSize() != 1)
         throw WalletException("invalid aet length");

      default_aet_ = (AddressEntryType)*defaultAetRef.getPtr();
   }

   {
      //top used index
      BinaryWriter bwKey;
      bwKey.put_uint32_t(TOPUSEDINDEX_KEY);
      auto topIndexRef = getDataRefForKey(bwKey.getData());

      if (topIndexRef.getSize() != 4)
         throw WalletException("invalid topindex length");

      BinaryRefReader brr(topIndexRef);
      highestUsedAddressIndex_.store(brr.get_int32_t(), memory_order_relaxed);
   }

   {
      //root asset
      BinaryWriter bwKey;
      bwKey.put_uint32_t(ROOTASSET_KEY);
      auto rootAssetRef = getDataRefForKey(bwKey.getData());

      root_ = AssetEntry::deserDBValue(-1, rootAssetRef);
   }

   {
      //asset entries
      auto dbIter = db_->begin();

      BinaryWriter bwKey;
      bwKey.put_uint8_t(ASSETENTRY_PREFIX);
      CharacterArrayRef keyRef(bwKey.getSize(), bwKey.getData().getPtr());

      dbIter.seek(keyRef, LMDB::Iterator::Seek_GE);

      while (dbIter.isValid())
      {
         auto iterkey = dbIter.key();
         auto itervalue = dbIter.value();

         BinaryDataRef keyBDR((uint8_t*)iterkey.mv_data, iterkey.mv_size);
         BinaryDataRef valueBDR((uint8_t*)itervalue.mv_data, itervalue.mv_size);

         //check value's advertized size is packet size and strip it
         BinaryRefReader brrVal(valueBDR);
         auto valsize = brrVal.get_var_int();
         if (valsize != brrVal.getSizeRemaining())
            throw WalletException("entry val size mismatch");
         
         try
         {
            auto entryPtr = AssetEntry::deserialize(keyBDR, 
               brrVal.get_BinaryDataRef(brrVal.getSizeRemaining()));
            assets_.insert(make_pair(entryPtr->getId(), entryPtr));
         }
         catch (AssetDeserException& e)
         {
            LOGERR << e.what();
            break;
         }

         dbIter.advance();
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
void AssetWallet_Multisig::readFromFile()
{
   //sanity check
   if (dbEnv_ == nullptr || db_ == nullptr)
      throw WalletException("uninitialized wallet object");

   {
      LMDBEnv::Transaction tx(dbEnv_, LMDB::ReadOnly);

      {
         //parentId
         BinaryWriter bwKey;
         bwKey.put_uint32_t(PARENTID_KEY);

         auto parentIdRef = getDataRefForKey(bwKey.getData());
         parentID_ = parentIdRef;
      }

      {
         //walletId
         BinaryWriter bwKey;
         bwKey.put_uint32_t(WALLETID_KEY);
         auto walletIdRef = getDataRefForKey(bwKey.getData());

         walletID_ = walletIdRef;
      }

      {
         //default AddressEntryType
         BinaryWriter bwKey;
         bwKey.put_uint32_t(ADDRESSENTRYTYPE_KEY);
         auto defaultAetRef = getDataRefForKey(bwKey.getData());

         if (defaultAetRef.getSize() != 1)
            throw WalletException("invalid aet length");

         default_aet_ = (AddressEntryType)*defaultAetRef.getPtr();
      }

      {
         //top used index
         BinaryWriter bwKey;
         bwKey.put_uint32_t(TOPUSEDINDEX_KEY);
         auto topIndexRef = getDataRefForKey(bwKey.getData());

         if (topIndexRef.getSize() != 4)
            throw WalletException("invalid topindex length");

         BinaryRefReader brr(topIndexRef);
         highestUsedAddressIndex_.store(brr.get_int32_t(), memory_order_relaxed);
      }

      {
         //derivation scheme
         BinaryWriter bwKey;
         bwKey.put_uint32_t(DERIVATIONSCHEME_KEY);
         auto derSchemeRef = getDataRefForKey(bwKey.getData());

         derScheme_ = DerivationScheme::deserialize(derSchemeRef);
      }

      {
         //lookup
         {
            BinaryWriter bwKey;
            bwKey.put_uint8_t(ASSETENTRY_PREFIX);
            auto lookupRef = getDataRefForKey(bwKey.getData());

            BinaryRefReader brr(lookupRef);
            chainLength_ = brr.get_uint32_t();
         }
      }
   }

   {
      //sub wallets
      auto derSchemeMS =
         dynamic_pointer_cast<DerivationScheme_Multisig>(derScheme_);

      if (derSchemeMS == nullptr)
         throw WalletException("unexpected derScheme ptr type");

      auto n = derSchemeMS->getN();

      map<BinaryData, shared_ptr<AssetWallet_Single>> walletPtrs;
      for (unsigned i = 0; i < n; i++)
      {
         stringstream ss;
         ss << "Subwallet-" << i;

         auto subwalletPtr = make_shared<AssetWallet_Single>(dbEnv_, ss.str());
         subwalletPtr->readFromFile();
         walletPtrs[subwalletPtr->getID()] = subwalletPtr;

      }

      derSchemeMS->setSubwalletPointers(walletPtrs);
   }

   {
      auto derSchemeMS = dynamic_pointer_cast<DerivationScheme_Multisig>(derScheme_);
      if (derSchemeMS == nullptr)
         throw WalletException("unexpected derScheme type");

      //build AssetEntry map
      for (unsigned i = 0; i < chainLength_; i++)
         assets_[i] = derSchemeMS->getAssetForIndex(i);
   }
}

////////////////////////////////////////////////////////////////////////////////
void AssetWallet::putData(const BinaryData& key, const BinaryData& data)
{
   /** the caller is responsible for the db transaction **/

   CharacterArrayRef keyRef(key.getSize(), key.getPtr());
   CharacterArrayRef dataRef(data.getSize(), data.getPtr());

   db_->insert(keyRef, dataRef);
}

////////////////////////////////////////////////////////////////////////////////
void AssetWallet::putData(BinaryWriter& key, BinaryWriter& data)
{
   putData(key.getData(), data.getData());
}

////////////////////////////////////////////////////////////////////////////////
unsigned AssetWallet::getAndBumpHighestUsedIndex()
{
   LMDBEnv::Transaction tx(dbEnv_, LMDB::ReadWrite);

   auto index = highestUsedAddressIndex_.fetch_add(1, memory_order_relaxed);

   BinaryWriter bwKey;
   bwKey.put_uint32_t(TOPUSEDINDEX_KEY);

   BinaryWriter bwData;
   bwData.put_var_int(4);
   bwData.put_int32_t(highestUsedAddressIndex_.load(memory_order_relaxed));

   putData(bwKey, bwData);

   return index;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AssetWallet::getNewAddress()
{
   //increment top used address counter & update
   auto index = getAndBumpHighestUsedIndex();

   //lock
   unique_lock<mutex> walletLock(walletMutex_);
   
   auto addrIter = addresses_.find(index);
   if (addrIter != addresses_.end())
      return addrIter->second;

   //check look up
   auto entryIter = assets_.find(index);
   if (entryIter == assets_.end())
   {
      if (assets_.size() == 0)
         throw WalletException("uninitialized wallet");
      extendChain(DERIVATION_LOOKUP);

      entryIter = assets_.find(index);
      if (entryIter == assets_.end())
         throw WalletException("requested index overflows max lookup");
   }
   
   auto aePtr = getAddressEntryForAsset(entryIter->second, default_aet_);

   //insert new entry
   addresses_[aePtr->getIndex()] = aePtr;

   return aePtr;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AssetWallet_Single::getAddressEntryForAsset(
   shared_ptr<AssetEntry> assetPtr, AddressEntryType ae_type)
{
   shared_ptr<AddressEntry> aePtr = nullptr;

   switch (ae_type)
   {
   case AddressEntryType_P2PKH:
      aePtr = make_shared<AddressEntry_P2PKH>(assetPtr);
      break;

   case AddressEntryType_P2WPKH:
      aePtr = make_shared<AddressEntry_P2WPKH>(assetPtr);
      break;

   default:
      throw WalletException("unsupported address entry type");
   }

   return aePtr;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AddressEntry> AssetWallet_Multisig::getAddressEntryForAsset(
   shared_ptr<AssetEntry> assetPtr, AddressEntryType ae_type)
{
   shared_ptr<AddressEntry> aePtr = nullptr;

   switch (ae_type)
   {
   case AddressEntryType_P2SH:
      aePtr = make_shared<AddressEntry_P2SH>(assetPtr);
      break;

   case AddressEntryType_P2WSH:
      aePtr = make_shared<AddressEntry_P2WSH>(assetPtr);
      break;

   default:
      throw WalletException("unsupported address entry type");
   }

   return aePtr;
}

////////////////////////////////////////////////////////////////////////////////
void AssetWallet::writeAssetEntry(shared_ptr<AssetEntry> entryPtr)
{
   auto&& serializedEntry = entryPtr->serialize();
   auto&& dbKey = entryPtr->getDbKey();

   CharacterArrayRef keyRef(dbKey.getSize(), dbKey.getPtr());
   CharacterArrayRef dataRef(serializedEntry.getSize(), serializedEntry.getPtr());

   LMDBEnv::Transaction(dbEnv_, LMDB::ReadWrite);
   db_->insert(keyRef, dataRef);
}

////////////////////////////////////////////////////////////////////////////////
vector<BinaryData> AssetWallet_Single::getAddrHashVec() const
{
   vector<BinaryData> h160Vec;

   for (auto& entry : assets_)
   {
      auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(entry.second);
      BinaryWriter bwUnc;
      bwUnc.put_uint8_t(BlockDataManagerConfig::getPubkeyHashPrefix());
      bwUnc.put_BinaryData(assetSingle->getHash160Uncompressed());

      BinaryWriter bwCmp;
      bwCmp.put_uint8_t(BlockDataManagerConfig::getPubkeyHashPrefix());
      bwCmp.put_BinaryData(assetSingle->getHash160Compressed());

      h160Vec.push_back(move(bwUnc.getData()));
      h160Vec.push_back(move(bwCmp.getData()));
   }

   return h160Vec;
}

////////////////////////////////////////////////////////////////////////////////
vector<BinaryData> AssetWallet_Single::getHash160VecUncompressed() const
{
   vector<BinaryData> h160Vec;

   for (auto& entry : assets_)
   {
      auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(entry.second);
      BinaryWriter bwUnc;
      bwUnc.put_uint8_t(BlockDataManagerConfig::getPubkeyHashPrefix());
      bwUnc.put_BinaryData(assetSingle->getHash160Uncompressed());

      h160Vec.push_back(move(bwUnc.getData()));
   }

   return h160Vec;
}

////////////////////////////////////////////////////////////////////////////////
vector<BinaryData> AssetWallet_Single::getHash160VecCompressed() const
{
   vector<BinaryData> h160Vec;

   for (auto& entry : assets_)
   {
      auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(entry.second);

      BinaryWriter bwCmp;
      bwCmp.put_uint8_t(BlockDataManagerConfig::getPubkeyHashPrefix());
      bwCmp.put_BinaryData(assetSingle->getHash160Compressed());

      h160Vec.push_back(move(bwCmp.getData()));
   }

   return h160Vec;
}

////////////////////////////////////////////////////////////////////////////////
vector<BinaryData> AssetWallet_Multisig::getAddrHashVec(void) const
{
   vector<BinaryData> hashVec;

   switch (default_aet_)
   {
   case AddressEntryType_P2SH:
   {
      for (auto& entry : assets_)
      {
         auto assetMS = dynamic_pointer_cast<AssetEntry_Multisig>(entry.second);
         BinaryWriter bw;
         bw.put_uint8_t(BlockDataManagerConfig::getScriptHashPrefix());
         bw.put_BinaryData(assetMS->getHash160());

         hashVec.push_back(move(bw.getData()));
      }
      
      break;
   }

   case AddressEntryType_P2WSH:
   {
      for (auto& entry : assets_)
      {
         auto assetMS = dynamic_pointer_cast<AssetEntry_Multisig>(entry.second);
         BinaryWriter bw;
         bw.put_uint8_t(BlockDataManagerConfig::getScriptHashPrefix());
         bw.put_BinaryData(assetMS->getHash256());

         hashVec.push_back(move(bw.getData()));
      }

      break;
   }

   default:
      throw WalletException("unexpected AddressEntryType for MS wallet");
   }
   return hashVec;
}


////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AssetWallet::getAssetForIndex(unsigned index) const
{
   auto iter = assets_.find(index);
   if (iter == assets_.end())
      throw WalletException("invalid asset index");

   return iter->second;
}

////////////////////////////////////////////////////////////////////////////////
string AssetWallet::getID(void) const
{
   return string(walletID_.getCharPtr(), walletID_.getSize());
}

////////////////////////////////////////////////////////////////////////////////
void AssetWallet::extendChain(unsigned count)
{
   if (assets_.size() == 0)
      throw WalletException("empty asset map");

   unique_lock<mutex> walletLock(walletMutex_, defer_lock);
   if (!walletLock.owns_lock())
      walletLock.lock();

   extendChain(assets_.rbegin()->second, count);
}

////////////////////////////////////////////////////////////////////////////////
void AssetWallet::extendChain(shared_ptr<AssetEntry> assetPtr, unsigned count)
{
   unique_lock<mutex> walletLock(walletMutex_, defer_lock);
   if (!walletLock.owns_lock())
      walletLock.lock();

   auto&& assetVec = derScheme_->extendChain(assetPtr, count);

   LMDBEnv::Transaction tx(dbEnv_, LMDB::ReadWrite);

   {
      for (auto& asset : assetVec)
      {
         auto id = asset->getId();
         auto iter = assets_.find(id);
         if (iter != assets_.end())
            continue;

         writeAssetEntry(asset);
         assets_.insert(make_pair(
            id, asset));
      }
   }
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// DerivationScheme
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
DerivationScheme::~DerivationScheme() 
{}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<DerivationScheme> DerivationScheme::deserialize(BinaryDataRef data)
{
   BinaryRefReader brr(data);

   //get derivation scheme type
   auto schemeType = brr.get_uint8_t();

   shared_ptr<DerivationScheme> derScheme;

   switch (schemeType)
   {
   case DERIVATIONSCHEME_LEGACY:
   {
      //get chaincode;
      auto len = brr.get_var_int();
      auto&& chainCode = SecureBinaryData(brr.get_BinaryDataRef(len));
      derScheme = make_shared<DerivationScheme_ArmoryLegacy>(move(chainCode));

      break;
   }

   case DERIVATIONSCHEME_MULTISIG:
   {
      //grab n, m
      auto m = brr.get_uint32_t();
      auto n = brr.get_uint32_t();

      set<BinaryData> ids;
      while (brr.getSizeRemaining() > 0)
      {
         auto len = brr.get_var_int();
         auto&& id = brr.get_BinaryData(len);
         ids.insert(move(id));
      }

      if (ids.size() != n)
         throw DerivationSchemeDeserException("id count mismatch");

      derScheme = make_shared<DerivationScheme_Multisig>(ids, n, m);

      break;
   }

   default:
      throw DerivationSchemeDeserException("unsupported derivation scheme");
   }

   return derScheme;
}

////////////////////////////////////////////////////////////////////////////////
vector<shared_ptr<AssetEntry>> DerivationScheme_ArmoryLegacy::extendChain(
   shared_ptr<AssetEntry> firstAsset, unsigned count)
{
   auto nextAsset = [this](
      shared_ptr<AssetEntry> assetPtr)->shared_ptr<AssetEntry>
   {
      auto assetSingle =
         dynamic_pointer_cast<AssetEntry_Single>(assetPtr);

      //get pubkey
      auto pubkey = assetSingle->getPubKey();
      auto& pubkeyData = pubkey->getUncompressedKey();

      auto&& nextPubkey = CryptoECDSA().ComputeChainedPublicKey(
         pubkeyData, chainCode_, nullptr);

      //try to get priv key
      auto privkey = assetSingle->getPrivKey();
      SecureBinaryData nextPrivkey;
      try
      {
         auto& privkeyData = privkey->getKey();

         nextPrivkey = move(CryptoECDSA().ComputeChainedPrivateKey(
            privkeyData, chainCode_, pubkeyData, nullptr));
      }
      catch (AssetUnavailableException&)
      {
         //no priv key, ignore
      }
      catch (CypherException&)
      {
         //ignore, not going to prompt user for password with priv key derivation
      }

      //no need to encrypt the new data, asset ctor will deal with it
      unique_ptr<Cypher> cypher;
      if (privkey->cypher_ != nullptr)
         cypher = move(privkey->cypher_->getCopy());

      return make_shared<AssetEntry_Single>(
         assetSingle->getId() + 1,
         move(nextPubkey), move(nextPrivkey), move(cypher));
   };
   
   vector<shared_ptr<AssetEntry>> assetVec;
   auto currentAsset = firstAsset;

   for (unsigned i = 0; i < count; i++)
   { 
      currentAsset = nextAsset(currentAsset);
      assetVec.push_back(currentAsset);
   }

   return assetVec;
}

////////////////////////////////////////////////////////////////////////////////
vector<shared_ptr<AssetEntry>> DerivationScheme_Multisig::extendChain(
   shared_ptr<AssetEntry> firstAsset, unsigned count)
{
   //synchronize wallet chains length
   unsigned bottom = UINT32_MAX;
   auto total = firstAsset->getId() + 1 + count; 
 
   for (auto& wltPtr : wallets_)
   {
      wltPtr.second->extendChain(
         total - wltPtr.second->getAssetCount());
   }

   vector<shared_ptr<AssetEntry>> assetVec;
   for (unsigned i = firstAsset->getId() + 1; i < total; i++)
      assetVec.push_back(getAssetForIndex(i));

   return assetVec;
}

////////////////////////////////////////////////////////////////////////////////
BinaryData DerivationScheme_ArmoryLegacy::serialize() const
{
   BinaryWriter bw;
   bw.put_uint8_t(DERIVATIONSCHEME_LEGACY);
   bw.put_var_int(chainCode_.getSize());
   bw.put_BinaryData(chainCode_);

   BinaryWriter final;
   final.put_var_int(bw.getSize());
   final.put_BinaryData(bw.getData());

   return final.getData();
}

////////////////////////////////////////////////////////////////////////////////
BinaryData DerivationScheme_Multisig::serialize() const
{
   if (walletIDs_.size() != n_)
      throw WalletException("multisig wallet is missing subwallets");

   BinaryWriter bw;
   bw.put_uint8_t(DERIVATIONSCHEME_MULTISIG);
   bw.put_uint32_t(m_);
   bw.put_uint32_t(n_);
   
   for (auto& id : walletIDs_)
   {
      bw.put_var_int(id.getSize());
      bw.put_BinaryData(id);
   }

   BinaryWriter bwFinal;
   bwFinal.put_var_int(bw.getSize());
   bwFinal.put_BinaryData(bw.getData());

   return bwFinal.getData();
}

////////////////////////////////////////////////////////////////////////////////
void DerivationScheme_Multisig::setSubwalletPointers(
   map<BinaryData, shared_ptr<AssetWallet_Single>> ptrMap)
{
   set<BinaryData> ids;
   for (auto& wltPtr : ptrMap)
      ids.insert(wltPtr.first);

   if (ids != walletIDs_)
      throw DerivationSchemeDeserException("ids set mismatch");

   wallets_ = ptrMap;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry_Multisig> DerivationScheme_Multisig::getAssetForIndex(
   unsigned index) const
{
   //gather assets
   map<BinaryData, shared_ptr<AssetEntry>> assetMap;

   for (auto wltPtr : wallets_)
   {
      auto asset = wltPtr.second->getAssetForIndex(index);
      assetMap.insert(make_pair(wltPtr.first, asset));
   }

   //create asset
   return make_shared<AssetEntry_Multisig>(index, assetMap, m_, n_);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AddressEntry
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
AddressEntry::~AddressEntry()
{}

////////////////////////////////////////////////////////////////////////////////
const BinaryData& AddressEntry_P2PKH::getAddress() const
{
   if (address_.getSize() == 0)
   {
      auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(asset_);
      if (assetSingle == nullptr)
         throw WalletException("unexpected asset entry type");

      auto& h160 = assetSingle->getHash160Uncompressed();

      //get and prepend network byte
      auto networkByte = BlockDataManagerConfig::getPubkeyHashPrefix();

      BinaryData addr160;
      addr160.append(networkByte);

      //b58 encode
      address_ = move(BtcUtils::scrAddrToBase58(addr160));
   }

   return address_;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<ScriptRecipient> AddressEntry_P2PKH::getRecipient(
   uint64_t value) const
{
   auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(asset_);
   if (assetSingle == nullptr)
      throw WalletException("unexpected asset entry type");

   auto& h160 = assetSingle->getHash160Uncompressed();
   return make_shared<Recipient_P2PKH>(h160, value);
}

////////////////////////////////////////////////////////////////////////////////
const BinaryData& AddressEntry_P2WPKH::getAddress() const
{
   if (address_.getSize() == 0)
   {
      auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(asset_);
      if (assetSingle == nullptr)
         throw WalletException("unexpected asset entry type");

      //no address standard for SW yet, consider BIP142
      address_ = assetSingle->getHash160Compressed();
   }

   return address_;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<ScriptRecipient> AddressEntry_P2WPKH::getRecipient(
   uint64_t value) const
{
   auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(asset_);
   if (assetSingle == nullptr)
      throw WalletException("unexpected asset entry type");

   auto& h160 = assetSingle->getHash160Compressed();
   return make_shared<Recipient_P2WPKH>(h160, value);
}

////////////////////////////////////////////////////////////////////////////////
const BinaryData& AddressEntry_P2SH::getAddress() const
{
   if (address_.getSize() == 0)
   {
      switch (asset_->getType())
      {
      case AssetEntryType_Single:
      {
         auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(asset_);
         if (assetSingle == nullptr)
            throw WalletException("unexpected asset entry type");

         //no address standard for SW yet, consider BIP142
         address_.append(BlockDataManagerConfig::getScriptHashPrefix());
         address_.append(assetSingle->getHash160Compressed());
         break;
      }

      case AssetEntryType_Multisig:
      {
         auto assetMS = dynamic_pointer_cast<AssetEntry_Multisig>(asset_);
         if (assetMS == nullptr)
            throw WalletException("unexpected asset entry type");

         address_.append(BlockDataManagerConfig::getScriptHashPrefix());
         address_.append(assetMS->getHash160());
         break;
      }
      
      default:
         throw WalletException("unexpected asset type");
      }

      address_ = move(BtcUtils::scrAddrToBase58(address_));
   }

   return address_;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<ScriptRecipient> AddressEntry_P2SH::getRecipient(
   uint64_t value) const
{
   BinaryDataRef h160;
   switch (asset_->getType())
   {
   case AssetEntryType_Single:
   {
      auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(asset_);
      if (assetSingle == nullptr)
         throw WalletException("unexpected asset entry type");

      h160 = assetSingle->getHash160Compressed().getRef();
      break;
   }

   case AssetEntryType_Multisig:
   {
      auto assetMS = dynamic_pointer_cast<AssetEntry_Multisig>(asset_);
      if (assetMS == nullptr)
         throw WalletException("unexpected asset entry type");

      h160 = assetMS->getHash160();
      break;
   }

   default:
      throw WalletException("unexpected asset type");
   }

   return make_shared<Recipient_P2SH>(h160, value);
}

////////////////////////////////////////////////////////////////////////////////
const BinaryData& AddressEntry_P2WSH::getAddress() const
{
   if (address_.getSize() == 0)
   {
      switch (asset_->getType())
      {
      case AssetEntryType_Single:
      {
         auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(asset_);
         if (assetSingle == nullptr)
            throw WalletException("unexpected asset entry type");

         //no address standard for SW yet, consider BIP142
         address_.append(BlockDataManagerConfig::getScriptHashPrefix());
         address_.append(assetSingle->getHash256Compressed());
         break;
      }

      case AssetEntryType_Multisig:
      {
         auto assetMS = dynamic_pointer_cast<AssetEntry_Multisig>(asset_);
         if (assetMS == nullptr)
            throw WalletException("unexpected asset entry type");

         address_.append(BlockDataManagerConfig::getScriptHashPrefix());
         address_.append(assetMS->getHash256());
         break;
      }

      default:
         throw WalletException("unexpected asset type");
      }

      //no address scheme for SW outputs yet
      //address_ = move(BtcUtils::scrAddrToBase58(address_));
   }

   return address_;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<ScriptRecipient> AddressEntry_P2WSH::getRecipient(
   uint64_t value) const
{
   BinaryDataRef h160;
   switch (asset_->getType())
   {
   case AssetEntryType_Single:
   {
      auto assetSingle = dynamic_pointer_cast<AssetEntry_Single>(asset_);
      if (assetSingle == nullptr)
         throw WalletException("unexpected asset entry type");

      h160 = assetSingle->getHash256Compressed().getRef();
      break;
   }

   case AssetEntryType_Multisig:
   {
      auto assetMS = dynamic_pointer_cast<AssetEntry_Multisig>(asset_);
      if (assetMS == nullptr)
         throw WalletException("unexpected asset entry type");

      h160 = assetMS->getHash256();
      break;
   }

   default:
      throw WalletException("unexpected asset type");
   }

   return make_shared<Recipient_PW2SH>(h160, value);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// Asset
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
Asset::~Asset()
{}

////////////////////////////////////////////////////////////////////////////////
BinaryData Asset_PublicKey::serialize() const
{
   BinaryWriter bw;
  
   bw.put_var_int(uncompressed_.getSize() + 1);
   bw.put_uint8_t(PUBKEY_UNCOMPRESSED_BYTE);
   bw.put_BinaryData(uncompressed_);

   bw.put_var_int(compressed_.getSize() + 1);
   bw.put_uint8_t(PUBKEY_COMPRESSED_BYTE);
   bw.put_BinaryData(compressed_);

   return bw.getData();
}

////////////////////////////////////////////////////////////////////////////////
BinaryData Asset_PrivateKey::serialize() const
{
   BinaryWriter bw;

   bw.put_var_int(data_.getSize() + 1);
   bw.put_uint8_t(PRIVKEY_BYTE);
   bw.put_BinaryData(data_);

   auto&& cypherData = cypher_->serialize();
   bw.put_var_int(cypherData.getSize());
   bw.put_BinaryData(cypherData);

   return bw.getData();
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// AssetEntry
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
AssetEntry::~AssetEntry(void)
{}

////////////////////////////////////////////////////////////////////////////////
const BinaryData& AssetEntry_Single::getHash160Uncompressed() const
{
   if (h160Uncompressed_.getSize() == 0)
      h160Uncompressed_ = 
         move(BtcUtils::getHash160(pubkey_->getUncompressedKey()));

   return h160Uncompressed_;
}

////////////////////////////////////////////////////////////////////////////////
const BinaryData& AssetEntry_Single::getHash160Compressed() const
{
   if (h160Compressed_.getSize() == 0)
      h160Compressed_ =
      move(BtcUtils::getHash160(pubkey_->getCompressedKey()));

   return h160Compressed_;
}

////////////////////////////////////////////////////////////////////////////////
const BinaryData& AssetEntry_Single::getHash256Compressed() const
{
   if (h256Compressed_.getSize() == 0)
      h256Compressed_ =
      move(BtcUtils::getHash256(pubkey_->getCompressedKey()));

   return h256Compressed_;
}


////////////////////////////////////////////////////////////////////////////////
const BinaryData& AssetEntry_Multisig::getScript() const
{
   if (multisigScript_.getSize() == 0)
   {
      BinaryWriter bw;

      //convert m to opcode and push
      auto m = m_ + OP_1 - 1;
      if (m > OP_16)
         throw WalletException("m exceeds OP_16");
      bw.put_uint8_t(m);

      //put pub keys
      for (auto& asset : assetMap_)
      {
         auto assetSingle =
            dynamic_pointer_cast<AssetEntry_Single>(asset.second);

         if (assetSingle == nullptr)
            WalletException("unexpected asset entry type");

         //using compressed keys
         auto& pubkeyCpr = assetSingle->getPubKey()->getCompressedKey();
         if (pubkeyCpr.getSize() != 33)
            throw WalletException("unexpected compress pub key len");

         bw.put_uint8_t(33);
         bw.put_BinaryData(pubkeyCpr);
      }

      //convert n to opcode and push
      auto n = n_ + OP_1 - 1;
      if (n > OP_16 || n < m)
         throw WalletException("invalid n");
      bw.put_uint8_t(n);

      bw.put_uint8_t(OP_CHECKMULTISIG);
      multisigScript_ = bw.getData();
   }

   return multisigScript_;
}

////////////////////////////////////////////////////////////////////////////////
const BinaryData& AssetEntry_Multisig::getHash160() const
{
   if (assetMap_.size() != n_)
      throw WalletException("asset count mismatch in multisig entry");


   if (h160_.getSize() == 0)
   {
      auto& msScript = getScript();
      h160_ = move(BtcUtils::getHash160(msScript));
   }

   return h160_;
}

////////////////////////////////////////////////////////////////////////////////
const BinaryData& AssetEntry_Multisig::getHash256() const
{
   if (assetMap_.size() != n_)
      throw WalletException("asset count mismatch in multisig entry");


   if (h256_.getSize() == 0)
   {
      auto& msScript = getScript();
      h256_ = move(BtcUtils::getSha256(msScript));
   }

   return h256_;
}

////////////////////////////////////////////////////////////////////////////////
BinaryData AssetEntry::getDbKey() const
{
   BinaryWriter bw;
   bw.put_uint8_t(ASSETENTRY_PREFIX);
   bw.put_int32_t(index_);

   return bw.getData();
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AssetEntry::deserialize(
   BinaryDataRef key, BinaryDataRef value)
{
   BinaryRefReader brrKey(key);

   auto prefix = brrKey.get_uint8_t();
   if (prefix != ASSETENTRY_PREFIX)
      throw AssetDeserException("invalid prefix");

   auto index = brrKey.get_int32_t();

   return deserDBValue(index, value);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<AssetEntry> AssetEntry::deserDBValue(int index, BinaryDataRef value)
{
   BinaryRefReader brrVal(value);
   auto entryType = (AssetEntryType)brrVal.get_uint8_t();

   switch (entryType)
   {
   case AssetEntryType_Single:
   {
      SecureBinaryData privKey;
      SecureBinaryData pubKeyCompressed;
      SecureBinaryData pubKeyUncompressed;
      unique_ptr<Cypher> cypher;

      vector<BinaryDataRef> dataVec;

      while (brrVal.getSizeRemaining() > 0)
      {
         auto len = brrVal.get_var_int();
         auto valref = brrVal.get_BinaryDataRef(len);

         dataVec.push_back(valref);
      }

      for (auto& dataRef : dataVec)
      {
         BinaryRefReader brrData(dataRef);
         auto keybyte = brrData.get_uint8_t();

         switch (keybyte)
         {
         case PUBKEY_UNCOMPRESSED_BYTE:
         {
            if (pubKeyUncompressed.getSize() != 0)
               throw AssetDeserException("multiple pub keys for entry");

            pubKeyUncompressed = move(SecureBinaryData(
               brrData.get_BinaryDataRef(
               brrData.getSizeRemaining())));

            break;
         }

         case PUBKEY_COMPRESSED_BYTE:
         {
            if (pubKeyCompressed.getSize() != 0)
               throw AssetDeserException("multiple pub keys for entry");

            pubKeyCompressed = move(SecureBinaryData(
               brrData.get_BinaryDataRef(
               brrData.getSizeRemaining())));

            break;
         }

         case PRIVKEY_BYTE:
         {
            if (privKey.getSize() != 0)
               throw AssetDeserException("multiple pub keys for entry");

            privKey = move(SecureBinaryData(
               brrData.get_BinaryDataRef(
               brrData.getSizeRemaining())));

            break;
         }

         case CYPHER_BYTE:
         {
            if (cypher != nullptr)
               throw AssetDeserException("multiple cyphers for entry");

            cypher = move(Cypher::deserialize(brrData));

            break;
         }

         default:
            throw AssetDeserException("unknown key type byte");
         }
      }

      //TODO: add IVs as args for encrypted entries
      return make_shared<AssetEntry_Single>(index, 
         move(pubKeyUncompressed), move(pubKeyCompressed), move(privKey), move(cypher));
   }

   default:
      throw AssetDeserException("invalid asset entry type");
   }
}

////////////////////////////////////////////////////////////////////////////////
BinaryData AssetEntry_Single::serialize() const
{
   BinaryWriter bw;
   bw.put_uint8_t(getType());

   bw.put_BinaryData(pubkey_->serialize());
   bw.put_BinaryData(privkey_->serialize());
   
   BinaryWriter finalBw;

   finalBw.put_var_int(bw.getSize());
   finalBw.put_BinaryData(bw.getData());

   return finalBw.getData();
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//// Cypher
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
Cypher::~Cypher()
{}

////////////////////////////////////////////////////////////////////////////////
unique_ptr<Cypher> Cypher::deserialize(BinaryRefReader& brr)
{
   unique_ptr<Cypher> cypher;
   auto type = brr.get_uint8_t();

   switch (type)
   {
   case CypherType_AES:
   {
      auto len = brr.get_var_int();
      auto&& iv = SecureBinaryData(brr.get_BinaryDataRef(len));

      cypher = move(make_unique<Cypher_AES>(move(iv)));

      break;
   }

   default:
      throw CypherException("unexpected cypher type");
   }

   return move(cypher);
}

////////////////////////////////////////////////////////////////////////////////
BinaryData Cypher_AES::serialize() const
{
   BinaryWriter bw;
   bw.put_uint8_t(CYPHER_BYTE);
   bw.put_uint8_t(getType());
   bw.put_var_int(iv_.getSize());
   bw.put_BinaryData(iv_);

   return bw.getData();
}

////////////////////////////////////////////////////////////////////////////////
unique_ptr<Cypher> Cypher_AES::getCopy() const
{
   return make_unique<Cypher_AES>();
}