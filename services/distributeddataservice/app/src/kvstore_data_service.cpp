/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
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

#define LOG_TAG "KvStoreDataService"

#include "kvstore_data_service.h"
#include <chrono>
#include <ctime>
#include <directory_ex.h>
#include <dirent.h>
#include <file_ex.h>
#include <ipc_skeleton.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <unistd.h>
#include "auto_launch_export.h"
#include "communication_provider.h"
#include "constant.h"
#include "crypto_utils.h"
#include "dds_trace.h"
#include "device_change_listener_impl.h"
#include "device_kvstore_impl.h"
#include "if_system_ability_manager.h"
#include "iservice_registry.h"
#include "kvstore_account_observer.h"
#include "kvstore_app_accessor.h"
#include "kvstore_meta_manager.h"
#include "kvstore_utils.h"
#include "log_print.h"
#include "permission/permission.h"
#include "permission/permission_kit.h"
#include "permission_validator.h"
#include "process_communicator_impl.h"
#include "reporter.h"
#include "system_ability_definition.h"
#include "uninstaller/uninstaller.h"

namespace OHOS {
namespace DistributedKv {
using json = nlohmann::json;
using namespace std::chrono;
using namespace OHOS::Security::Permission;

REGISTER_SYSTEM_ABILITY_BY_ID(KvStoreDataService, DISTRIBUTED_KV_DATA_SERVICE_ABILITY_ID, true);

constexpr size_t MAX_APP_ID_LENGTH = 256;

KvStoreDataService::KvStoreDataService(bool runOnCreate)
    : SystemAbility(runOnCreate),
      accountMutex_(),
      deviceAccountMap_(),
      clientDeathObserverMutex_(),
      clientDeathObserverMap_()
{
    ZLOGI("begin.");
    Initialize();
}

KvStoreDataService::KvStoreDataService(int32_t systemAbilityId, bool runOnCreate)
    : SystemAbility(systemAbilityId, runOnCreate),
      accountMutex_(),
      deviceAccountMap_(),
      clientDeathObserverMutex_(),
      clientDeathObserverMap_()
{
    ZLOGI("begin");
    Initialize();
}

KvStoreDataService::~KvStoreDataService()
{
    ZLOGI("begin.");
    deviceAccountMap_.clear();
}

void KvStoreDataService::Initialize()
{
    ZLOGI("begin.");
    KvStoreMetaManager::GetInstance().InitMetaParameter();
    std::thread th = std::thread([]() {
        DistributedDB::KvStoreDelegateManager::SetProcessLabel(Constant::PROCESS_LABEL, "default");
        auto communicator = std::make_shared<AppDistributedKv::ProcessCommunicatorImpl>();
        auto ret = DistributedDB::KvStoreDelegateManager::SetProcessCommunicator(communicator);
        ZLOGI("set communicator ret:%d.", static_cast<int>(ret));
        if (KvStoreMetaManager::GetInstance().CheckRootKeyExist() == Status::SUCCESS) {
            return;
        }
        constexpr int RETRY_MAX_TIMES = 100;
        int retryCount = 0;
        constexpr int RETRY_TIME_INTERVAL_MILLISECOND = 1 * 1000 * 1000; // retry after 1 second
        while (retryCount < RETRY_MAX_TIMES) {
            if (KvStoreMetaManager::GetInstance().GenerateRootKey() == Status::SUCCESS) {
                ZLOGI("GenerateRootKey success.");
                break;
            }
            retryCount++;
            ZLOGE("GenerateRootKey failed.");
            usleep(RETRY_TIME_INTERVAL_MILLISECOND);
        }
    });
    th.detach();

    accountEventObserver_ = std::make_shared<KvStoreAccountObserver>(*this);
    AccountDelegate::GetInstance()->Subscribe(accountEventObserver_);
}

bool KvStoreDataService::CheckBundleName(const std::string &bundleName) const
{
    if (bundleName.empty() || bundleName.size() > MAX_APP_ID_LENGTH ||
        bundleName.find(Constant::KEY_SEPARATOR) != std::string::npos) {
        return false;
    }

    auto iter = std::find_if_not(bundleName.begin(), bundleName.end(),
        [](char c) { return (std::isprint(c) && c != '/'); });

    return (iter == bundleName.end());
}

bool KvStoreDataService::CheckStoreId(const std::string &storeId) const
{
    if (storeId.empty() || storeId.size() > Constant::MAX_STORE_ID_LENGTH ||
        storeId.find(Constant::KEY_SEPARATOR) != std::string::npos) {
        return false;
    }

    auto iter = std::find_if_not(storeId.begin(), storeId.end(),
        [](char c) { return (std::isdigit(c) || std::isalpha(c) || c == '_'); });

    return (iter == storeId.end());
}

Status KvStoreDataService::GetKvStore(const Options &options, const AppId &appId, const StoreId &storeId,
                                      std::function<void(sptr<IKvStoreImpl>)> callback)
{
    ZLOGI("begin.");
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    if (callback == nullptr) {
        ZLOGW("callback is nullptr");
        return Status::ERROR;
    }

    GetKvStorePara getKvStorePara;
    Status checkParaStatus = CheckParameters(options, appId, storeId, KvStoreType::MULTI_VERSION, getKvStorePara);
    if (checkParaStatus != Status::SUCCESS) {
        callback(nullptr);
        return checkParaStatus;
    }

    SecretKeyPara secretKeyParas;
    Status getSecretKeyStatus = KvStoreDataService::GetSecretKey(options, getKvStorePara, secretKeyParas);
    if (getSecretKeyStatus != Status::SUCCESS) {
        callback(nullptr);
        return getSecretKeyStatus;
    }

    auto deviceAccountId = getKvStorePara.deviceAccountId;
    auto bundleName = getKvStorePara.bundleName;
    auto storeIdTmp = getKvStorePara.storeId;
    auto secretKey = secretKeyParas.secretKey;
    bool outdated = secretKeyParas.outdated;

    auto it = deviceAccountMap_.find(deviceAccountId);
    if (it == deviceAccountMap_.end()) {
        auto result = deviceAccountMap_.emplace(std::piecewise_construct,
            std::forward_as_tuple(deviceAccountId), std::forward_as_tuple(deviceAccountId));
        if (!result.second) {
            ZLOGE("emplace failed.");
            FaultMsg msg = {FaultType::RUNTIME_FAULT, "user", __FUNCTION__, Fault::RF_GET_DB};
            Reporter::GetInstance()->ServiceFault()->Report(msg);
            callback(nullptr);
            return Status::ERROR;
        }
        it = result.first;
    }

    auto newCallback = [&callback, outdated, deviceAccountId, bundleName, storeIdTmp](sptr<IKvStoreImpl> store) {
        if (outdated) {
            KvStoreMetaManager::GetInstance().ReKey(deviceAccountId, bundleName, storeIdTmp, store);
        }
        callback(store);
    };
    Status statusTmp = (it->second).GetKvStore(options, bundleName, storeIdTmp, secretKey, newCallback);

    ZLOGD("get kvstore return status:%d, deviceAccountId:[%s], bundleName:[%s].",
          statusTmp, KvStoreUtils::ToBeAnonymous(deviceAccountId).c_str(),  bundleName.c_str());
    if (statusTmp == Status::SUCCESS) {
        return UpdateMetaData(options, getKvStorePara, secretKeyParas.metaKey, it->second);
    }
    getKvStorePara.getKvStoreStatus = statusTmp;
    return GetKvStoreFailDo(options, getKvStorePara, secretKeyParas, it->second, callback);
}

Status KvStoreDataService::GetSingleKvStore(const Options &options, const AppId &appId, const StoreId &storeId,
                                            std::function<void(sptr<ISingleKvStore>)> callback)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    ZLOGI("begin.");
    if (callback == nullptr) {
        ZLOGW("callback is nullptr");
        return Status::ERROR;
    }

    GetKvStorePara getKvStorePara;
    getKvStorePara.funType = KvStoreType::SINGLE_VERSION;
    Status checkParaStatus = CheckParameters(options, appId, storeId, KvStoreType::SINGLE_VERSION, getKvStorePara);
    if (checkParaStatus != Status::SUCCESS) {
        callback(nullptr);
        return checkParaStatus;
    }

    SecretKeyPara secretKeyParas;
    Status getSecretKeyStatus = KvStoreDataService::GetSecretKey(options, getKvStorePara, secretKeyParas);
    if (getSecretKeyStatus != Status::SUCCESS) {
        callback(nullptr);
        return getSecretKeyStatus;
    }

    auto deviceAccountId = getKvStorePara.deviceAccountId;
    auto bundleName = getKvStorePara.bundleName;
    auto storeIdTmp = getKvStorePara.storeId;

    auto secretKey = secretKeyParas.secretKey;
    bool outdated = secretKeyParas.outdated;

    auto it = deviceAccountMap_.find(deviceAccountId);
    if (it == deviceAccountMap_.end()) {
        auto result = deviceAccountMap_.emplace(std::piecewise_construct,
            std::forward_as_tuple(deviceAccountId), std::forward_as_tuple(deviceAccountId));
        if (!result.second) {
            ZLOGE("emplace failed.");
            callback(nullptr);
            return Status::ERROR;
        }
        it = result.first;
    }
    auto newCallback = [&callback, outdated, deviceAccountId, bundleName, storeIdTmp](sptr<ISingleKvStore> store) {
        if (outdated) {
            KvStoreMetaManager::GetInstance().ReKey(deviceAccountId, bundleName, storeIdTmp, store);
        }
        callback(store);
    };
    Status statusTmp = (it->second).GetSingleKvStore(options, bundleName, storeIdTmp, secretKey, newCallback);
    if (statusTmp == Status::SUCCESS) {
        return UpdateMetaData(options, getKvStorePara, secretKeyParas.metaKey, it->second);
    }
    getKvStorePara.getKvStoreStatus = statusTmp;
    return GetSingleKvStoreFailDo(options, getKvStorePara, secretKeyParas, it->second, callback);
}

Status KvStoreDataService::CheckParameters(const Options &options, const AppId &appId,
    const StoreId &storeId, const KvStoreType &kvStoreType, GetKvStorePara &getKvStorePara)
{
    if (appId.appId.empty() || storeId.storeId.empty()) {
        ZLOGW("appid or storeid empty");
        return Status::INVALID_ARGUMENT;
    }

    KvStoreType kvStoreTypeInOptions = options.kvStoreType;
    if (kvStoreTypeInOptions != KvStoreType::DEVICE_COLLABORATION && kvStoreTypeInOptions != kvStoreType) {
        ZLOGE("invalid kvStore type.");
        return Status::INVALID_ARGUMENT;
    }
    KVSTORE_ACCOUNT_EVENT_PROCESSING_CHECKER(Status::SYSTEM_ACCOUNT_EVENT_PROCESSING);
    std::string bundleName = Constant::TrimCopy<std::string>(appId.appId);
    std::string storeIdTmp = Constant::TrimCopy<std::string>(storeId.storeId);
    if (!CheckBundleName(bundleName)) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }
    if (!CheckStoreId(storeIdTmp)) {
        ZLOGE("invalid storeIdTmp.");
        return Status::INVALID_ARGUMENT;
    }

    std::string trueAppId = KvStoreUtils::GetAppIdByBundleName(bundleName);
    if (trueAppId.empty()) {
        ZLOGW("appId empty(permission issues?)");
        return Status::PERMISSION_DENIED;
    }

    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string deviceAccountId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    if (deviceAccountId != AccountDelegate::MAIN_DEVICE_ACCOUNT_ID) {
        ZLOGE("not support sub account");
        return Status::NOT_SUPPORT;
    }

    GetKvStorePara KvStorePara;
    KvStorePara.bundleName = bundleName;
    KvStorePara.storeId = storeIdTmp;
    KvStorePara.trueAppId = trueAppId;
    KvStorePara.deviceAccountId = deviceAccountId;
    getKvStorePara = KvStorePara;

    return Status::SUCCESS;
}

Status KvStoreDataService::GetSecretKey(const Options &options, const GetKvStorePara &kvParas,
    SecretKeyPara &secretKeyParas)
{
    std::string bundleName = kvParas.bundleName;
    std::string storeIdTmp = kvParas.storeId;
    std::string deviceAccountId = kvParas.deviceAccountId;

    std::lock_guard<std::mutex> lg(accountMutex_);
    auto metaKey = KvStoreMetaManager::GetMetaKey(deviceAccountId, "default", bundleName, storeIdTmp);
    if (!CheckOptions(options, metaKey)) {
        ZLOGE("encrypt type or kvStore type is not the same");
        return Status::INVALID_ARGUMENT;
    }
    std::vector<uint8_t> secretKey;
    std::unique_ptr<std::vector<uint8_t>, void (*)(std::vector<uint8_t> *)> cleanGuard(
        &secretKey, [](std::vector<uint8_t> *ptr) { ptr->assign(ptr->size(), 0); });

    std::vector<uint8_t> metaSecretKey;
    std::string secretKeyFile;
    if (kvParas.funType == KvStoreType::MULTI_VERSION) {
        metaSecretKey = KvStoreMetaManager::GetMetaKey(deviceAccountId, "default", bundleName, storeIdTmp, "KEY");
        secretKeyFile = KvStoreMetaManager::GetSecretKeyFile(
            deviceAccountId, bundleName, storeIdTmp, options.securityLevel);
    } else {
        metaSecretKey = KvStoreMetaManager::GetMetaKey(deviceAccountId, "default", bundleName,
                                                       storeIdTmp, "SINGLE_KEY");
        secretKeyFile = KvStoreMetaManager::GetSecretSingleKeyFile(
            deviceAccountId, bundleName, storeIdTmp, options.securityLevel);
    }

    bool outdated = false;
    Status alreadyCreated = KvStoreMetaManager::GetInstance().CheckUpdateServiceMeta(metaSecretKey, CHECK_EXIST_LOCAL);
    if (options.encrypt) {
        ZLOGI("Getting secret key");
        Status recStatus = RecoverSecretKey(alreadyCreated, outdated, metaSecretKey, secretKey, secretKeyFile);
        if (recStatus != Status::SUCCESS) {
            return recStatus;
        }
    } else {
        if (alreadyCreated == Status::SUCCESS || FileExists(secretKeyFile)) {
            ZLOGW("try to get an encrypted store with false option encrypt parameter");
            return Status::CRYPT_ERROR;
        }
    }

    SecretKeyPara kvStoreSecretKey;
    kvStoreSecretKey.metaKey = metaKey;
    kvStoreSecretKey.secretKey = secretKey;
    kvStoreSecretKey.metaSecretKey = metaSecretKey;
    kvStoreSecretKey.secretKeyFile = secretKeyFile;
    kvStoreSecretKey.alreadyCreated = alreadyCreated;
    kvStoreSecretKey.outdated = outdated;
    secretKeyParas = kvStoreSecretKey;

    return Status::SUCCESS;
}

Status KvStoreDataService::RecoverSecretKey(const Status &alreadyCreated, bool &outdated,
    const std::vector<uint8_t> &metaSecretKey, std::vector<uint8_t> &secretKey, const std::string &secretKeyFile)
{
    if (alreadyCreated != Status::SUCCESS) {
        KvStoreMetaManager::GetInstance().RecoverSecretKeyFromFile(
            secretKeyFile, metaSecretKey, secretKey, outdated);
        if (secretKey.empty()) {
            ZLOGI("new secret key");
            CryptoUtils::GetRandomKey(32, secretKey); // 32 is key length
            KvStoreMetaManager::GetInstance().WriteSecretKeyToMeta(metaSecretKey, secretKey);
            KvStoreMetaManager::GetInstance().WriteSecretKeyToFile(secretKeyFile, secretKey);
        }
    } else {
        KvStoreMetaManager::GetInstance().GetSecretKeyFromMeta(metaSecretKey, secretKey, outdated);
        if (secretKey.empty()) {
            ZLOGW("get secret key from meta failed, try to recover");
            KvStoreMetaManager::GetInstance().RecoverSecretKeyFromFile(
                secretKeyFile, metaSecretKey, secretKey, outdated);
        }
        if (secretKey.empty()) {
            ZLOGW("recover failed");
            return Status::CRYPT_ERROR;
        }
        KvStoreMetaManager::GetInstance().WriteSecretKeyToFile(secretKeyFile, secretKey);
    }
    return Status::SUCCESS;
}

Status KvStoreDataService::UpdateMetaData(const Options &options, const GetKvStorePara &kvParas,
    const std::vector<uint8_t> &metaKey, KvStoreUserManager &kvStoreUserManager)
{
    KvStoreMetaData metaData;
    metaData.appId = kvParas.trueAppId;
    metaData.appType = "harmony";
    metaData.bundleName = kvParas.bundleName;
    metaData.deviceAccountId = kvParas.deviceAccountId;
    metaData.deviceId = DeviceKvStoreImpl::GetLocalDeviceId();
    metaData.isAutoSync = options.autoSync;
    metaData.isBackup = options.backup;
    metaData.isEncrypt = options.encrypt;
    metaData.kvStoreType = options.kvStoreType;
    metaData.schema = options.schema;
    metaData.storeId = kvParas.storeId;
    metaData.userId = AccountDelegate::GetInstance()->GetCurrentHarmonyAccountId(kvParas.bundleName);
    metaData.uid = IPCSkeleton::GetCallingUid();
    metaData.version = KVSTORE_META_VERSION;
    metaData.securityLevel = options.securityLevel;
    if (kvParas.funType == KvStoreType::MULTI_VERSION) {
        metaData.dataDir = "default";
    } else {
        metaData.dataDir = kvStoreUserManager.GetDbDir(kvParas.bundleName, options);
    }

    std::string jsonStr = metaData.Marshal();
    std::vector<uint8_t> jsonVec(jsonStr.begin(), jsonStr.end());

    return KvStoreMetaManager::GetInstance().CheckUpdateServiceMeta(metaKey, UPDATE, jsonVec);
}

Status KvStoreDataService::GetKvStoreFailDo(const Options &options, const GetKvStorePara &kvParas,
    SecretKeyPara &secKeyParas, KvStoreUserManager &kvUserManager, std::function<void(sptr<IKvStoreImpl>)> callback)
{
    Status statusTmp = kvParas.getKvStoreStatus;
    Status getKvStoreStatus = statusTmp;
    ZLOGW("getKvStore failed with status %d", static_cast<int>(getKvStoreStatus));
    if (getKvStoreStatus == Status::CRYPT_ERROR && options.encrypt) {
        if (secKeyParas.alreadyCreated != Status::SUCCESS) {
            // create encrypted store failed, remove secret key
            KvStoreMetaManager::GetInstance().RemoveSecretKey(kvParas.deviceAccountId, kvParas.bundleName,
                                                              kvParas.storeId);
            return Status::ERROR;
        }
        // get existing encrypted store failed, retry with key stored in file
        Status status = KvStoreMetaManager::GetInstance().RecoverSecretKeyFromFile(
            secKeyParas.secretKeyFile, secKeyParas.metaSecretKey, secKeyParas.secretKey, secKeyParas.outdated);
        if (status != Status::SUCCESS) {
            callback(nullptr);
            return Status::CRYPT_ERROR;
        }
        // here callback is called twice
        statusTmp = kvUserManager.GetKvStore(options, kvParas.bundleName, kvParas.storeId, secKeyParas.secretKey,
            [&](sptr<IKvStoreImpl> store) {
                if (secKeyParas.outdated) {
                    KvStoreMetaManager::GetInstance().ReKey(kvParas.deviceAccountId, kvParas.bundleName,
                        kvParas.storeId, store);
                }
                callback(store);
            });
    }

    // if kvstore damaged and no backup file, then return DB_ERROR
    if (statusTmp != Status::SUCCESS && getKvStoreStatus == Status::CRYPT_ERROR) {
        // if backup file not exist, dont need recover
        if (!CheckBackupFileExist(kvParas.deviceAccountId, kvParas.bundleName, kvParas.storeId,
                                  options.securityLevel)) {
            return Status::CRYPT_ERROR;
        }
        // remove damaged database
        if (DeleteKvStoreOnly(kvParas.storeId, kvParas.deviceAccountId, kvParas.bundleName) != Status::SUCCESS) {
            ZLOGE("DeleteKvStoreOnly failed.");
            return Status::DB_ERROR;
        }
        // recover database
        return RecoverMultiKvStore(options, kvParas.bundleName, kvParas.storeId, secKeyParas.secretKey, callback);
    }
    return statusTmp;
}

Status KvStoreDataService::GetSingleKvStoreFailDo(const Options &options, const GetKvStorePara &kvParas,
                                                  SecretKeyPara &secKeyParas, KvStoreUserManager &kvUserManager,
                                                  std::function<void(sptr<ISingleKvStore>)> callback)
{
    Status statusTmp = kvParas.getKvStoreStatus;
    Status getKvStoreStatus = statusTmp;
    ZLOGW("getKvStore failed with status %d", static_cast<int>(getKvStoreStatus));
    if (getKvStoreStatus == Status::CRYPT_ERROR && options.encrypt) {
        if (secKeyParas.alreadyCreated != Status::SUCCESS) {
            // create encrypted store failed, remove secret key
            KvStoreMetaManager::GetInstance().RemoveSecretKey(kvParas.deviceAccountId, kvParas.bundleName,
                                                              kvParas.storeId);
            return Status::ERROR;
        }
        // get existing encrypted store failed, retry with key stored in file
        Status status = KvStoreMetaManager::GetInstance().RecoverSecretKeyFromFile(
            secKeyParas.secretKeyFile, secKeyParas.metaSecretKey, secKeyParas.secretKey, secKeyParas.outdated);
        if (status != Status::SUCCESS) {
            callback(nullptr);
            return Status::CRYPT_ERROR;
        }
        // here callback is called twice
        statusTmp = kvUserManager.GetSingleKvStore(options, kvParas.bundleName, kvParas.storeId, secKeyParas.secretKey,
            [&](sptr<ISingleKvStore> store) {
                if (secKeyParas.outdated) {
                    KvStoreMetaManager::GetInstance().ReKey(kvParas.deviceAccountId, kvParas.bundleName,
                        kvParas.storeId, store);
                }
                callback(store);
            });
    }

    // if kvstore damaged and no backup file, then return DB_ERROR
    if (statusTmp != Status::SUCCESS && getKvStoreStatus == Status::CRYPT_ERROR) {
        // if backup file not exist, dont need recover
        if (!CheckBackupFileExist(kvParas.deviceAccountId, kvParas.bundleName, kvParas.storeId,
                                  options.securityLevel)) {
            return Status::CRYPT_ERROR;
        }
        // remove damaged database
        if (DeleteKvStoreOnly(kvParas.storeId, kvParas.deviceAccountId, kvParas.bundleName) != Status::SUCCESS) {
            ZLOGE("DeleteKvStoreOnly failed.");
            return Status::DB_ERROR;
        }
        // recover database
        return RecoverSingleKvStore(options, kvParas.bundleName, kvParas.storeId, secKeyParas.secretKey, callback);
    }
    return statusTmp;
}

bool KvStoreDataService::CheckOptions(const Options &options, const std::vector<uint8_t> &metaKey) const
{
    ZLOGI("begin.");
    KvStoreMetaData metaData;
    metaData.version = 0;
    Status statusTmp = KvStoreMetaManager::GetInstance().GetKvStoreMeta(metaKey, metaData);
    if (statusTmp == Status::KEY_NOT_FOUND) {
        ZLOGI("get metaKey not found.");
        return true;
    }
    if (statusTmp != Status::SUCCESS) {
        ZLOGE("get metaKey failed.");
        return false;
    }
    ZLOGI("metaData encrypt is %d, kvStore type is %d, options encrypt is %d, kvStore type is %d",
          static_cast<int>(metaData.isEncrypt), static_cast<int>(metaData.kvStoreType),
          static_cast<int>(options.encrypt), static_cast<int>(options.kvStoreType));
    if (options.encrypt != metaData.isEncrypt) {
        ZLOGE("checkOptions encrypt type is not the same.");
        return false;
    }

    if (options.kvStoreType != metaData.kvStoreType && metaData.version != 0) {
        ZLOGE("checkOptions kvStoreType is not the same.");
        return false;
    }
    ZLOGI("end.");
    return true;
}

bool KvStoreDataService::CheckBackupFileExist(const std::string &deviceAccountId, const std::string &bundleName,
                                              const std::string &storeId, int securityLevel)
{
    auto pathType = KvStoreAppManager::ConvertPathType(bundleName, securityLevel);
    std::initializer_list<std::string> backupFileNameList = {Constant::DEFAULT_GROUP_ID, "_",
        bundleName, "_", storeId};
    auto backupFileName = Constant::Concatenate(backupFileNameList);
    std::initializer_list<std::string> backFileList = {BackupHandler::GetBackupPath(deviceAccountId, pathType),
        "/", BackupHandler::GetHashedBackupName(backupFileName)};
    auto backFilePath = Constant::Concatenate(backFileList);
    if (!BackupHandler::FileExists(backFilePath)) {
        ZLOGE("BackupHandler file is not exist.");
        return false;
    }
    return true;
}

Status KvStoreDataService::RecoverSingleKvStore(const Options &options,
                                                const std::string &bundleName,
                                                const std::string &storeId,
                                                const std::vector<uint8_t> &secretKey,
                                                std::function<void(sptr<ISingleKvStore>)> callback)
{
    // restore database
    std::string storeIdTmp = storeId;
    std::string trueAppId = KvStoreUtils::GetAppIdByBundleName(bundleName);
    Options optionsTmp = options;
    optionsTmp.createIfMissing = true;

    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string deviceAccountId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    auto it = deviceAccountMap_.find(deviceAccountId);
    if (it == deviceAccountMap_.end()) {
        ZLOGD("deviceAccountId not found");
        return Status::INVALID_ARGUMENT;
    }

    sptr<ISingleKvStore> kvStorePtr;
    Status statusTmp = (it->second).GetSingleKvStore(
        optionsTmp, bundleName, storeIdTmp, secretKey,
        [&kvStorePtr](sptr<ISingleKvStore> store) { kvStorePtr = store; });
    // restore database failed
    if (statusTmp != Status::SUCCESS || kvStorePtr == nullptr) {
        ZLOGE("RecoverSingleKvStore reget GetSingleKvStore failed.");
        return Status::DB_ERROR;
    }
    // recover database from backup file
    auto kvStorePtrTmp = static_cast<SingleKvStoreImpl *>(kvStorePtr.GetRefPtr());
    bool importRet = kvStorePtrTmp->Import(bundleName);
    callback(kvStorePtr);
    if (!importRet) {
        ZLOGE("RecoverSingleKvStore Import failed.");
        return Status::RECOVER_FAILED;
    }
    ZLOGD("RecoverSingleKvStore Import success.");
    return Status::RECOVER_SUCCESS;
}

Status KvStoreDataService::RecoverMultiKvStore(const Options &options,
                                               const std::string &bundleName,
                                               const std::string &storeId,
                                               const std::vector<uint8_t> &secretKey,
                                               std::function<void(sptr<IKvStoreImpl>)> callback)
{
    // restore database
    std::string storeIdTmp = storeId;
    std::string trueAppId = KvStoreUtils::GetAppIdByBundleName(bundleName);
    Options optionsTmp = options;
    optionsTmp.createIfMissing = true;

    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string deviceAccountId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    auto it = deviceAccountMap_.find(deviceAccountId);
    if (it == deviceAccountMap_.end()) {
        ZLOGD("deviceAccountId not found");
        return Status::INVALID_ARGUMENT;
    }

    sptr<IKvStoreImpl> kvStorePtr;
    Status statusTmp = (it->second).GetKvStore(
        optionsTmp, bundleName, storeIdTmp, secretKey,
        [&kvStorePtr](sptr<IKvStoreImpl> store) {
            kvStorePtr = store;
        });
    // restore database failed
    if (statusTmp != Status::SUCCESS || kvStorePtr == nullptr) {
        ZLOGE("RecoverMultiKvStore reget GetSingleKvStore failed.");
        return Status::DB_ERROR;
    }

    // recover database from backup file
    auto kvStorePtrTmp = static_cast<KvStoreImpl *>(kvStorePtr.GetRefPtr());
    if (!kvStorePtrTmp->Import(bundleName)) {
        ZLOGE("RecoverMultiKvStore Import failed.");
        return Status::RECOVER_FAILED;
    }
    ZLOGD("RecoverMultiKvStore Import success.");
    callback(kvStorePtr);
    return Status::RECOVER_SUCCESS;
}

void KvStoreDataService::GetAllKvStoreId(
    const AppId &appId, std::function<void(Status, std::vector<StoreId> &)> callback)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    ZLOGI("GetAllKvStoreId begin.");
    std::string bundleName = Constant::TrimCopy<std::string>(appId.appId);
    std::vector<StoreId> storeIdList;
    if (bundleName.empty() || bundleName.size() > MAX_APP_ID_LENGTH) {
        ZLOGE("invalid appId.");
        callback(Status::INVALID_ARGUMENT, storeIdList);
        return;
    }

    auto &metaKvStoreDelegate = KvStoreMetaManager::GetInstance().GetMetaKvStore();
    if (metaKvStoreDelegate == nullptr) {
        ZLOGE("metaKvStoreDelegate is null");
        callback(Status::DB_ERROR, storeIdList);
        return;
    }

    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string deviceAccountId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    if (deviceAccountId != AccountDelegate::MAIN_DEVICE_ACCOUNT_ID) {
        ZLOGE("not support sub account");
        return;
    }
    std::vector<DistributedDB::Entry> dbEntries;
    DistributedDB::DBStatus dbStatus;
    DistributedDB::Key dbKey = KvStoreMetaRow::GetKeyFor(DeviceKvStoreImpl::GetLocalDeviceId() +
        Constant::KEY_SEPARATOR + deviceAccountId + Constant::KEY_SEPARATOR +
        "default" + Constant::KEY_SEPARATOR + bundleName + Constant::KEY_SEPARATOR);
    DdsTrace traceDelegate(std::string(LOG_TAG "Delegate::") + std::string(__FUNCTION__));
    dbStatus = metaKvStoreDelegate->GetEntries(dbKey, dbEntries);
    if (dbStatus != DistributedDB::DBStatus::OK) {
        ZLOGE("GetEntries delegate return error: %d.", static_cast<int>(dbStatus));
        if (dbEntries.empty()) {
            callback(Status::SUCCESS, storeIdList);
        } else {
            callback(Status::DB_ERROR, storeIdList);
        }
        return;
    }

    for (const auto &entry : dbEntries) {
        std::string keyStr = std::string(entry.key.begin(), entry.key.end());
        size_t pos = keyStr.find_last_of(Constant::KEY_SEPARATOR);
        if (pos == std::string::npos) {
            continue;
        }
        StoreId storeId;
        storeId.storeId = keyStr.substr(pos + 1);
        storeIdList.push_back(storeId);
    }
    callback(Status::SUCCESS, storeIdList);
}

Status KvStoreDataService::CloseKvStore(const AppId &appId, const StoreId &storeId)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    ZLOGI("begin.");
    std::string bundleName = Constant::TrimCopy<std::string>(appId.appId);
    std::string storeIdTmp = Constant::TrimCopy<std::string>(storeId.storeId);
    if (!CheckBundleName(bundleName)) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }
    if (!CheckStoreId(storeIdTmp)) {
        ZLOGE("invalid storeIdTmp.");
        return Status::INVALID_ARGUMENT;
    }

    std::string trueAppId = KvStoreUtils::GetAppIdByBundleName(bundleName);
    if (trueAppId.empty()) {
        ZLOGE("get appId failed.");
        return Status::PERMISSION_DENIED;
    }

    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string deviceAccountId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    if (deviceAccountId != AccountDelegate::MAIN_DEVICE_ACCOUNT_ID) {
        ZLOGE("not support sub account");
        return Status::NOT_SUPPORT;
    }
    std::lock_guard<std::mutex> lg(accountMutex_);
    auto it = deviceAccountMap_.find(deviceAccountId);
    if (it != deviceAccountMap_.end()) {
        Status status = (it->second).CloseKvStore(bundleName, storeIdTmp);
        if (status != Status::STORE_NOT_OPEN) {
            return status;
        }
    }
    FaultMsg msg = {FaultType::RUNTIME_FAULT, "user", __FUNCTION__, Fault::RF_CLOSE_DB};
    Reporter::GetInstance()->ServiceFault()->Report(msg);
    ZLOGE("return STORE_NOT_OPEN.");
    return Status::STORE_NOT_OPEN;
}

/* close all opened kvstore */
Status KvStoreDataService::CloseAllKvStore(const AppId &appId)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    ZLOGD("begin.");
    std::string bundleName = Constant::TrimCopy<std::string>(appId.appId);
    if (!CheckBundleName(bundleName)) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }

    std::string trueAppId = KvStoreUtils::GetAppIdByBundleName(bundleName);
    if (trueAppId.empty()) {
        ZLOGE("get appId failed.");
        return Status::PERMISSION_DENIED;
    }

    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string deviceAccountId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    if (deviceAccountId != AccountDelegate::MAIN_DEVICE_ACCOUNT_ID) {
        ZLOGE("not support sub account");
        return Status::NOT_SUPPORT;
    }
    std::lock_guard<std::mutex> lg(accountMutex_);
    auto it = deviceAccountMap_.find(deviceAccountId);
    if (it != deviceAccountMap_.end()) {
        return (it->second).CloseAllKvStore(bundleName);
    }
    ZLOGE("store not open.");
    return Status::STORE_NOT_OPEN;
}

Status KvStoreDataService::DeleteKvStore(const AppId &appId, const StoreId &storeId)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::string bundleName = appId.appId;
    if (!CheckBundleName(bundleName)) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }
    std::string trueAppId = KvStoreUtils::GetAppIdByBundleName(bundleName);
    if (trueAppId.empty()) {
        ZLOGE("get appId failed.");
        return Status::PERMISSION_DENIED;
    }
    // delete the backup file
    std::initializer_list<std::string> backFileList = {
        AccountDelegate::GetInstance()->GetCurrentHarmonyAccountId(), "_", bundleName, "_", storeId.storeId};
    auto backupFileName = Constant::Concatenate(backFileList);

    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string deviceAccountId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    if (deviceAccountId != AccountDelegate::MAIN_DEVICE_ACCOUNT_ID) {
        ZLOGE("not support sub account");
        return Status::NOT_SUPPORT;
    }

    std::initializer_list<std::string> backPathListDE = {BackupHandler::GetBackupPath(deviceAccountId,
        KvStoreAppManager::PATH_DE), "/", BackupHandler::GetHashedBackupName(backupFileName)};
    auto backFilePath = Constant::Concatenate(backPathListDE);
    if (!BackupHandler::RemoveFile(backFilePath)) {
        ZLOGE("DeleteKvStore RemoveFile backFilePath failed.");
    }
    std::initializer_list<std::string> backPathListCE = {BackupHandler::GetBackupPath(deviceAccountId,
        KvStoreAppManager::PATH_CE), "/", BackupHandler::GetHashedBackupName(backupFileName)};
    backFilePath = Constant::Concatenate(backPathListCE);
    if (!BackupHandler::RemoveFile(backFilePath)) {
        ZLOGE("DeleteKvStore RemoveFile backFilePath failed.");
    }
    return DeleteKvStore(appId, storeId, bundleName);
}

/* delete all kv store */
Status KvStoreDataService::DeleteAllKvStore(const AppId &appId)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));

    ZLOGI("%s", appId.appId.c_str());
    std::string bundleName = Constant::TrimCopy<std::string>(appId.appId);
    if (!CheckBundleName(bundleName)) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }
    if (KvStoreUtils::GetAppIdByBundleName(bundleName).empty()) {
        ZLOGE("invalid appId.");
        return Status::PERMISSION_DENIED;
    }
    Status statusTmp;
    std::vector<StoreId> existStoreIds;
    GetAllKvStoreId(appId, [&statusTmp, &existStoreIds](Status status, std::vector<StoreId> &storeIds) {
        statusTmp = status;
        existStoreIds = std::move(storeIds);
    });

    if (statusTmp != Status::SUCCESS) {
        ZLOGE("%s, error: %d ", bundleName.c_str(), static_cast<int>(statusTmp));
        return statusTmp;
    }

    for (const auto &storeId : existStoreIds) {
        statusTmp = DeleteKvStore(appId, storeId);
        if (statusTmp != Status::SUCCESS) {
            ZLOGE("%s, error: %d ", bundleName.c_str(), static_cast<int>(statusTmp));
            return statusTmp;
        }
    }

    return statusTmp;
}

/* RegisterClientDeathObserver */
Status KvStoreDataService::RegisterClientDeathObserver(const AppId &appId, sptr<IRemoteObject> observer)
{
    ZLOGD("begin.");
    KVSTORE_ACCOUNT_EVENT_PROCESSING_CHECKER(Status::SYSTEM_ACCOUNT_EVENT_PROCESSING);
    std::string bundleName = Constant::TrimCopy<std::string>(appId.appId);
    if (!CheckBundleName(bundleName)) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }
    std::string trueAppId = KvStoreUtils::GetAppIdByBundleName(bundleName);
    if (trueAppId.empty()) {
        return Status::PERMISSION_DENIED;
    }
    std::lock_guard<std::mutex> lg(clientDeathObserverMutex_);
    auto it = clientDeathObserverMap_.emplace(std::piecewise_construct, std::forward_as_tuple(bundleName),
        std::forward_as_tuple(appId, *this, std::move(observer)));
    ZLOGI("map size: %zu.", clientDeathObserverMap_.size());
    if (!it.second) {
        ZLOGI("insert failed");
        return Status::ERROR;
    }
    ZLOGI("insert success");
    const std::string userId = AccountDelegate::GetInstance()->GetCurrentHarmonyAccountId();
    KvStoreTuple kvStoreTuple {userId, trueAppId};
    AppThreadInfo appThreadInfo {IPCSkeleton::GetCallingPid(), IPCSkeleton::GetCallingUid()};
    PermissionValidator::RegisterPermissionChanged(kvStoreTuple, appThreadInfo);
    return Status::SUCCESS;
}

Status KvStoreDataService::AppExit(const AppId &appId)
{
    ZLOGI("AppExit");
    // memory of parameter appId locates in a member of clientDeathObserverMap_ and will be freed after
    // clientDeathObserverMap_ erase, so we have to take a copy if we want to use this parameter after erase operation.
    AppId appIdTmp = appId;
    {
        std::lock_guard<std::mutex> lg(clientDeathObserverMutex_);
        clientDeathObserverMap_.erase(appIdTmp.appId);
        ZLOGI("map size: %zu.", clientDeathObserverMap_.size());
    }

    std::string trueAppId = KvStoreUtils::GetAppIdByBundleName(appIdTmp.appId);
    if (trueAppId.empty()) {
        ZLOGE("get appid for KvStore failed because of permission denied.");
        return Status::PERMISSION_DENIED;
    }
    const std::string userId = AccountDelegate::GetInstance()->GetCurrentHarmonyAccountId(appIdTmp.appId);
    KvStoreTuple kvStoreTuple {userId, trueAppId};
    PermissionValidator::UnregisterPermissionChanged(kvStoreTuple);

    CloseAllKvStore(appIdTmp);
    return Status::SUCCESS;
}

void KvStoreDataService::OnDump()
{
    ZLOGD("begin.");
}

int KvStoreDataService::Dump(int fd, const std::vector<std::u16string> &args)
{
    int uid = static_cast<int>(IPCSkeleton::GetCallingUid());
    const int maxUid = 10000;
    if (uid > maxUid) {
        return 0;
    }
    dprintf(fd, "------------------------------------------------------------------\n");
    dprintf(fd, "DeviceAccount count : %u\n", static_cast<uint32_t>(deviceAccountMap_.size()));
    for (const auto &pair : deviceAccountMap_) {
        dprintf(fd, "DeviceAccountID    : %s\n", KvStoreUtils::GetAppIdByBundleName(pair.first).c_str());
        pair.second.Dump(fd);
    }
    return 0;
}

const std::string PKGNAME = "ohos.distributeddata";
const std::string APP_DATASYNC_PERMISSION = "ohos.permission.DISTRIBUTED_DATASYNC";
const std::string LABEL = "distributeddata";
const std::string DESCRIPTION = "distributeddata service";
const int LABEL_ID = 9527;
const int DESCRIPTION_ID = 9528;

void KvStoreDataService::AddPermission() const
{
    std::vector<PermissionDef> permissionDefs {
        {
            .permissionName = APP_DATASYNC_PERMISSION,
            .bundleName = PKGNAME,
            .grantMode = GrantMode::SYSTEM_GRANT,
            .availableScope = AVAILABLE_SCOPE_ALL,
            .label = LABEL,
            .labelId = LABEL_ID,
            .description = DESCRIPTION,
            .descriptionId = DESCRIPTION_ID
        }
    };
    PermissionKit::AddDefPermissions(permissionDefs);
    std::vector<std::string> permissions;
    permissions.push_back(APP_DATASYNC_PERMISSION);
    PermissionKit::AddSystemGrantedReqPermissions(PKGNAME, permissions);
    PermissionKit::GrantSystemGrantedPermission(PKGNAME, APP_DATASYNC_PERMISSION);
}

void KvStoreDataService::OnStart()
{
    ZLOGI("distributeddata service onStart");
    auto samgr = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgr != nullptr) {
        ZLOGI("samgr exist.");
        auto remote = samgr->CheckSystemAbility(DISTRIBUTED_KV_DATA_SERVICE_ABILITY_ID);
        auto kvDataServiceProxy = iface_cast<IKvStoreDataService>(remote);
        if (kvDataServiceProxy != nullptr) {
            ZLOGI("service has been registered.");
            return;
        }
    }
    StartService();
}

void KvStoreDataService::StartService()
{
    // register this to ServiceManager.
    bool ret = SystemAbility::Publish(this);
    if (!ret) {
        FaultMsg msg = {FaultType::SERVICE_FAULT, "service", __FUNCTION__, Fault::SF_SERVICE_PUBLISH};
        Reporter::GetInstance()->ServiceFault()->Report(msg);
    }
    Uninstaller::GetInstance().Init(this);

    // add softbus permission.
    AddPermission();

    std::string backupPath = BackupHandler::GetBackupPath(AccountDelegate::MAIN_DEVICE_ACCOUNT_ID,
                                                          KvStoreAppManager::PATH_DE);
    ZLOGI("backupPath is : %s ", backupPath.c_str());
    if (!ForceCreateDirectory(backupPath)) {
        ZLOGE("backup create directory failed");
    }
    // Initialize meta db delegate manager.
    KvStoreMetaManager::GetInstance().InitMetaListener([this](const KvStoreMetaData &metaData) {
        if (!metaData.isDirty) {
            return;
        }

        AppId appId;
        appId.appId = metaData.bundleName;
        StoreId storeId;
        storeId.storeId = metaData.storeId;
        CloseKvStore(appId, storeId);
        DeleteKvStore(appId, storeId);
    });

    // subscribe account event listener to EventNotificationMgr
    AccountDelegate::GetInstance()->SubscribeAccountEvent();
    auto permissionCheckCallback =
        [this](const std::string &userId, const std::string &appId, const std::string
        &storeId, const std::string &deviceId, uint8_t flag) -> bool {
            // temp add permission whilelist for ddmp; this should be config in ddmp manifest.
            ZLOGD("checking sync permission start appid:%s, stid:%s.", appId.c_str(), storeId.c_str());
            return CheckPermissions(userId, appId, storeId, deviceId, flag);
        };
    auto dbStatus = DistributedDB::KvStoreDelegateManager::SetPermissionCheckCallback(permissionCheckCallback);
    if (dbStatus != DistributedDB::DBStatus::OK) {
        ZLOGE("SetPermissionCheck callback failed.");
    }
    ZLOGI("autoLaunchRequestCallback start");
    auto autoLaunchRequestCallback =
        [this](const std::string &identifier, DistributedDB::AutoLaunchParam &param) -> bool {
            ResolveAutoLaunchParamByIdentifier(identifier, param);
            return true;
        };
    DistributedDB::KvStoreDelegateManager::SetAutoLaunchRequestCallback(autoLaunchRequestCallback);

    backup_ = std::make_unique<BackupHandler>(this);
    backup_->BackSchedule();

    std::thread th = std::thread([]() {
        sleep(TEN_SEC);
        KvStoreAppAccessor::GetInstance().EnableKvStoreAutoLaunch();
    });
    th.detach();
    ZLOGI("Publish ret: %d", static_cast<int>(ret));
}

void KvStoreDataService::ResolveAutoLaunchParamByIdentifier(const std::string &identifier,
                                                            DistributedDB::AutoLaunchParam &param)
{
    ZLOGI("start");
    std::map<std::string, MetaData> entries;
    if (KvStoreMetaManager::GetInstance().GetFullMetaData(entries)) {
        for (const auto &entry : entries) {
            const std::string userId = AccountDelegate::GetInstance()->GetCurrentHarmonyAccountId(
                entry.second.kvStoreMetaData.bundleName);
            const std::string &curIdentifier = DistributedDB::KvStoreDelegateManager::GetKvStoreIdentifier(userId,
                entry.second.kvStoreMetaData.appId, entry.second.kvStoreMetaData.storeId);
            if (identifier == curIdentifier) {
                ZLOGI("identifier  find");
                DistributedDB::AutoLaunchOption option;
                option.createIfNecessary = false;
                option.isEncryptedDb = entry.second.kvStoreMetaData.isEncrypt;
                DistributedDB::CipherPassword password;
                const std::vector<uint8_t> &secretKey = entry.second.secretKeyMetaData.secretKey;
                if (password.SetValue(secretKey.data(), secretKey.size()) != DistributedDB::CipherPassword::OK) {
                    ZLOGE("Get secret key failed.");
                }
                option.passwd = password;
                option.schema = entry.second.kvStoreMetaData.schema;
                option.createDirByStoreIdOnly = true;
                option.dataDir = entry.second.kvStoreMetaData.dataDir;
                option.secOption = KvStoreAppManager::ConvertSecurity(entry.second.kvStoreMetaData.securityLevel);
                param.userId = userId;
                param.appId = entry.second.kvStoreMetaData.appId;
                param.storeId = entry.second.kvStoreMetaData.storeId;
                param.option = option;
            }
        }
    }
}

bool KvStoreDataService::CheckPermissions(const std::string &userId, const std::string &appId,
                                          const std::string &storeId, const std::string &deviceId, uint8_t flag) const
{
    auto &instance = KvStoreMetaManager::GetInstance();
    KvStoreMetaData metaData;
    auto localDevId = DeviceKvStoreImpl::GetLocalDeviceId();
    auto qstatus = instance.QueryKvStoreMetaDataByDeviceIdAndAppId(localDevId, appId, metaData);
    if (qstatus != Status::SUCCESS) {
        qstatus = instance.QueryKvStoreMetaDataByDeviceIdAndAppId("", appId, metaData); // local device id maybe null
        if (qstatus != Status::SUCCESS) {
            ZLOGW("query appId failed.");
            return false;
        }
    }
    if (metaData.appType.compare("default") == 0) {
        ZLOGD("default, dont check sync permission.");
        return true;
    }
    Status status = instance.CheckSyncPermission(userId, appId, storeId, flag, deviceId);
    if (status != Status::SUCCESS) {
        ZLOGW("PermissionCheck failed.");
        return false;
    }

    if (metaData.appType.compare("harmony") != 0) {
        ZLOGD("it's A app, dont check sync permission.");
        return true;
    }

    if (PermissionValidator::IsAutoLaunchEnabled(appId)) {
        return true;
    }
    bool ret = PermissionValidator::CheckSyncPermission(userId, appId, metaData.uid);
    ZLOGD("checking sync permission ret:%d.", ret);
    return ret;
}

void KvStoreDataService::OnStop()
{
    ZLOGI("begin.");
    if (backup_ != nullptr) {
        backup_.reset();
        backup_ = nullptr;
    }
}

KvStoreDataService::KvStoreClientDeathObserverImpl::KvStoreClientDeathObserverImpl(
    const AppId &appId, KvStoreDataService &service, sptr<IRemoteObject> observer)
    : appId_(appId), dataService_(service), observerProxy_(std::move(observer)),
    deathRecipient_(new KvStoreDeathRecipient(*this))
{
    ZLOGI("KvStoreClientDeathObserverImpl");

    if (observerProxy_ != nullptr) {
        ZLOGI("add death recipient");
        observerProxy_->AddDeathRecipient(deathRecipient_);
    } else {
        ZLOGW("observerProxy_ is nullptr");
    }
}

KvStoreDataService::KvStoreClientDeathObserverImpl::~KvStoreClientDeathObserverImpl()
{
    ZLOGI("~KvStoreClientDeathObserverImpl");
    if (deathRecipient_ != nullptr && observerProxy_ != nullptr) {
        ZLOGI("remove death recipient");
        observerProxy_->RemoveDeathRecipient(deathRecipient_);
    }
}

void KvStoreDataService::KvStoreClientDeathObserverImpl::NotifyClientDie()
{
    ZLOGI("appId: %s", appId_.appId.c_str());
    dataService_.AppExit(appId_);
}

KvStoreDataService::KvStoreClientDeathObserverImpl::KvStoreDeathRecipient::KvStoreDeathRecipient(
    KvStoreClientDeathObserverImpl &kvStoreClientDeathObserverImpl)
    : kvStoreClientDeathObserverImpl_(kvStoreClientDeathObserverImpl)
{
    ZLOGI("KvStore Client Death Observer");
}

KvStoreDataService::KvStoreClientDeathObserverImpl::KvStoreDeathRecipient::~KvStoreDeathRecipient()
{
    ZLOGI("KvStore Client Death Observer");
}

void KvStoreDataService::KvStoreClientDeathObserverImpl::KvStoreDeathRecipient::OnRemoteDied(
    const wptr<IRemoteObject> &remote)
{
    ZLOGI("begin");
    kvStoreClientDeathObserverImpl_.NotifyClientDie();
}

Status KvStoreDataService::DeleteKvStore(const AppId &appId, const StoreId &storeId, const std::string &trueAppId)
{
    ZLOGI("begin.");
    std::string bundleName = Constant::TrimCopy<std::string>(appId.appId);
    std::string storeIdTmp = Constant::TrimCopy<std::string>(storeId.storeId);
    if (!CheckBundleName(bundleName)) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }
    if (!CheckStoreId(storeIdTmp)) {
        ZLOGE("invalid storeIdTmp.");
        return Status::INVALID_ARGUMENT;
    }
    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string deviceAccountId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    if (deviceAccountId != AccountDelegate::MAIN_DEVICE_ACCOUNT_ID) {
        ZLOGE("not support sub account");
        return Status::NOT_SUPPORT;
    }
    std::lock_guard<std::mutex> lg(accountMutex_);
    Status status;
    auto it = deviceAccountMap_.find(deviceAccountId);
    if (it != deviceAccountMap_.end()) {
        status = (it->second).DeleteKvStore(trueAppId, storeIdTmp);
    } else {
        KvStoreUserManager kvStoreUserManager(deviceAccountId);
        status = kvStoreUserManager.DeleteKvStore(trueAppId, storeIdTmp);
    }

    if (status == Status::SUCCESS) {
        auto metaKey = KvStoreMetaManager::GetMetaKey(deviceAccountId, "default", bundleName, storeIdTmp);
        status = KvStoreMetaManager::GetInstance().CheckUpdateServiceMeta(metaKey, DELETE);
        if (status != Status::SUCCESS) {
            ZLOGW("Remove Kvstore Metakey failed.");
        }
        KvStoreMetaManager::GetInstance().RemoveSecretKey(deviceAccountId, bundleName, storeIdTmp);
        KvStoreMetaManager::GetInstance().DeleteStrategyMeta(bundleName, storeIdTmp);
    }
    return status;
}


Status KvStoreDataService::DeleteKvStoreOnly(const std::string &storeIdTmp, const std::string &deviceAccountId,
                                             const std::string &bundleName)
{
    ZLOGI("DeleteKvStoreOnly begin.");
    auto it = deviceAccountMap_.find(deviceAccountId);
    if (it != deviceAccountMap_.end()) {
        return (it->second).DeleteKvStore(bundleName, storeIdTmp);
    }
    KvStoreUserManager kvStoreUserManager(deviceAccountId);
    return kvStoreUserManager.DeleteKvStore(bundleName, storeIdTmp);
}

void KvStoreDataService::AccountEventChanged(const AccountEventInfo &eventInfo)
{
    ZLOGI("account event %d changed process, begin.", eventInfo.status);
    std::lock_guard<std::mutex> lg(accountMutex_);
    switch (eventInfo.status) {
        case AccountStatus::HARMONY_ACCOUNT_LOGIN:
        case AccountStatus::HARMONY_ACCOUNT_LOGOUT: {
            g_kvStoreAccountEventStatus = 1;
            // migrate all kvstore belong to this device account
            for (auto &it : deviceAccountMap_) {
                (it.second).MigrateAllKvStore(eventInfo.harmonyAccountId);
            }
            g_kvStoreAccountEventStatus = 0;
            break;
        }
        case AccountStatus::DEVICE_ACCOUNT_DELETE: {
            g_kvStoreAccountEventStatus = 1;
            // delete all kvstore belong to this device account
            for (auto &it : deviceAccountMap_) {
                (it.second).DeleteAllKvStore();
            }
            auto it = deviceAccountMap_.find(eventInfo.deviceAccountId);
            if (it != deviceAccountMap_.end()) {
                deviceAccountMap_.erase(eventInfo.deviceAccountId);
            }
            std::initializer_list<std::string> dirList = {Constant::ROOT_PATH_DE, "/",
                Constant::SERVICE_NAME, "/", eventInfo.deviceAccountId};
            std::string deviceAccountKvStoreDataDir = Constant::Concatenate(dirList);
            ForceRemoveDirectory(deviceAccountKvStoreDataDir);
            dirList = {Constant::ROOT_PATH_CE, "/", Constant::SERVICE_NAME, "/", eventInfo.deviceAccountId};
            deviceAccountKvStoreDataDir = Constant::Concatenate(dirList);
            ForceRemoveDirectory(deviceAccountKvStoreDataDir);
            g_kvStoreAccountEventStatus = 0;
            break;
        }
        default: {
            break;
        }
    }
    ZLOGI("account event %d changed process, end.", eventInfo.status);
}

Status KvStoreDataService::GetLocalDevice(DeviceInfo &device)
{
    auto tmpDevice = KvStoreUtils::GetProviderInstance().GetLocalBasicInfo();
    device = {tmpDevice.deviceId, tmpDevice.deviceName, tmpDevice.deviceType};
    return Status::SUCCESS;
}

Status KvStoreDataService::GetDeviceList(std::vector<DeviceInfo> &deviceInfoList, DeviceFilterStrategy strategy)
{
    auto devices = KvStoreUtils::GetProviderInstance().GetRemoteNodesBasicInfo();
    for (auto const &device : devices) {
        DeviceInfo deviceInfo = {device.deviceId, device.deviceName, device.deviceType};
        deviceInfoList.push_back(deviceInfo);
    }
    ZLOGD("strategy is %d.", strategy);
    return Status::SUCCESS;
}

Status KvStoreDataService::StartWatchDeviceChange(sptr<IDeviceStatusChangeListener> observer,
                                                  DeviceFilterStrategy strategy)
{
    if (observer == nullptr) {
        ZLOGD("observer is null");
        return Status::INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lck(deviceListenerMutex_);
    if (deviceListener_ == nullptr) {
        deviceListener_ = std::make_shared<DeviceChangeListenerImpl>(deviceListeners_);
        KvStoreUtils::GetProviderInstance().StartWatchDeviceChange(deviceListener_.get(), {"serviceWatcher"});
    }
    IRemoteObject *objectPtr = observer->AsObject().GetRefPtr();
    auto listenerPair = std::make_pair(objectPtr, observer);
    deviceListeners_.insert(listenerPair);
    ZLOGD("strategy is %d.", strategy);
    return Status::SUCCESS;
}

Status KvStoreDataService::StopWatchDeviceChange(sptr<IDeviceStatusChangeListener> observer)
{
    if (observer == nullptr) {
        ZLOGD("observer is null");
        return Status::INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lck(deviceListenerMutex_);
    IRemoteObject *objectPtr = observer->AsObject().GetRefPtr();
    auto it = deviceListeners_.find(objectPtr);
    if (it == deviceListeners_.end()) {
        return Status::ILLEGAL_STATE;
    }
    deviceListeners_.erase(it->first);
    return Status::SUCCESS;
}

bool DbMetaCallbackDelegateMgr::GetKvStoreDiskSize(const std::string &storeId, uint64_t &size)
{
    if (IsDestruct()) {
        return false;
    }
    DistributedDB::DBStatus ret = delegate_->GetKvStoreDiskSize(storeId, size);
    return (ret == DistributedDB::DBStatus::OK);
}

void DbMetaCallbackDelegateMgr::GetKvStoreKeys(std::vector<StoreInfo> &dbStats)
{
    if (IsDestruct()) {
        return;
    }
    DistributedDB::DBStatus dbStatusTmp;
    Option option {.createIfNecessary = true, .isMemoryDb = false, .isEncryptedDb = false};
    DistributedDB::KvStoreNbDelegate *kvStoreNbDelegatePtr = nullptr;
    delegate_->GetKvStore(
        Constant::SERVICE_META_DB_NAME, option,
        [&kvStoreNbDelegatePtr, &dbStatusTmp](DistributedDB::DBStatus dbStatus,
                                              DistributedDB::KvStoreNbDelegate *kvStoreNbDelegate) {
            kvStoreNbDelegatePtr = kvStoreNbDelegate;
            dbStatusTmp = dbStatus;
        });

    if (dbStatusTmp != DistributedDB::DBStatus::OK) {
        return;
    }
    DistributedDB::Key dbKey = KvStoreMetaRow::GetKeyFor("");
    std::vector<DistributedDB::Entry> entries;
    kvStoreNbDelegatePtr->GetEntries(dbKey, entries);
    if (entries.empty()) {
        delegate_->CloseKvStore(kvStoreNbDelegatePtr);
        return;
    }
    for (auto const &entry : entries) {
        std::string key = std::string(entry.key.begin(), entry.key.end());
        std::vector<std::string> out;
        Split(key, Constant::KEY_SEPARATOR, out);
        if (out.size() >= VECTOR_SIZE) {
            StoreInfo storeInfo = {out[USER_ID], out[APP_ID], out[STORE_ID]};
            dbStats.push_back(std::move(storeInfo));
        }
    }
    delegate_->CloseKvStore(kvStoreNbDelegatePtr);
}
}  // namespace DistributedKv
}  // namespace OHOS
