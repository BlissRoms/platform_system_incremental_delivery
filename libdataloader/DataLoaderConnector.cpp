/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "incfs-dataloaderconnector"

#include <android-base/logging.h>
#include <nativehelper/JNIHelp.h>
#include <sys/stat.h>
#include <utils/Looper.h>

#include <thread>
#include <unordered_map>

#include "JNIHelpers.h"
#include "ManagedDataLoader.h"
#include "dataloader.h"
#include "incfs.h"

namespace {

using namespace android::dataloader;
using namespace std::literals;
using android::base::unique_fd;

using FileId = android::incfs::FileId;
using RawMetadata = android::incfs::RawMetadata;

struct JniIds {
    struct {
        jint DATA_LOADER_CREATED;
        jint DATA_LOADER_DESTROYED;
        jint DATA_LOADER_STARTED;
        jint DATA_LOADER_STOPPED;
        jint DATA_LOADER_IMAGE_READY;
        jint DATA_LOADER_IMAGE_NOT_READY;
        jint DATA_LOADER_SLOW_CONNECTION;
        jint DATA_LOADER_NO_CONNECTION;
        jint DATA_LOADER_CONNECTION_OK;
    } constants;

    jmethodID parcelFileDescriptorGetFileDescriptor;

    jfieldID incremental;
    jfieldID callback;

    jfieldID controlCmd;
    jfieldID controlPendingReads;
    jfieldID controlLog;

    jfieldID paramsType;
    jfieldID paramsPackageName;
    jfieldID paramsClassName;
    jfieldID paramsArguments;
    jfieldID paramsDynamicArgs;

    jfieldID namedFdFd;
    jfieldID namedFdName;

    jclass listener;
    jmethodID listenerOnStatusChanged;

    jmethodID callbackControlWriteData;

    JniIds(JNIEnv* env) {
        listener = (jclass)env->NewGlobalRef(
                FindClassOrDie(env, "android/content/pm/IDataLoaderStatusListener"));
        listenerOnStatusChanged = GetMethodIDOrDie(env, listener, "onStatusChanged", "(II)V");

        constants.DATA_LOADER_CREATED =
                GetStaticIntFieldValueOrDie(env, listener, "DATA_LOADER_CREATED");
        constants.DATA_LOADER_DESTROYED =
                GetStaticIntFieldValueOrDie(env, listener, "DATA_LOADER_DESTROYED");
        constants.DATA_LOADER_STARTED =
                GetStaticIntFieldValueOrDie(env, listener, "DATA_LOADER_STARTED");
        constants.DATA_LOADER_STOPPED =
                GetStaticIntFieldValueOrDie(env, listener, "DATA_LOADER_STOPPED");
        constants.DATA_LOADER_IMAGE_READY =
                GetStaticIntFieldValueOrDie(env, listener, "DATA_LOADER_IMAGE_READY");
        constants.DATA_LOADER_IMAGE_NOT_READY =
                GetStaticIntFieldValueOrDie(env, listener, "DATA_LOADER_IMAGE_NOT_READY");

        constants.DATA_LOADER_SLOW_CONNECTION =
                GetStaticIntFieldValueOrDie(env, listener, "DATA_LOADER_SLOW_CONNECTION");
        constants.DATA_LOADER_NO_CONNECTION =
                GetStaticIntFieldValueOrDie(env, listener, "DATA_LOADER_NO_CONNECTION");
        constants.DATA_LOADER_CONNECTION_OK =
                GetStaticIntFieldValueOrDie(env, listener, "DATA_LOADER_CONNECTION_OK");

        CHECK(constants.DATA_LOADER_SLOW_CONNECTION == DATA_LOADER_SLOW_CONNECTION);
        CHECK(constants.DATA_LOADER_NO_CONNECTION == DATA_LOADER_NO_CONNECTION);
        CHECK(constants.DATA_LOADER_CONNECTION_OK == DATA_LOADER_CONNECTION_OK);

        auto parcelFileDescriptor = FindClassOrDie(env, "android/os/ParcelFileDescriptor");
        parcelFileDescriptorGetFileDescriptor =
                GetMethodIDOrDie(env, parcelFileDescriptor, "getFileDescriptor",
                                 "()Ljava/io/FileDescriptor;");

        auto control = FindClassOrDie(env, "android/content/pm/FileSystemControlParcel");
        incremental =
                GetFieldIDOrDie(env, control, "incremental",
                                "Landroid/os/incremental/IncrementalFileSystemControlParcel;");
        callback =
                GetFieldIDOrDie(env, control, "callback",
                                "Landroid/content/pm/IPackageInstallerSessionFileSystemConnector;");

        auto incControl =
                FindClassOrDie(env, "android/os/incremental/IncrementalFileSystemControlParcel");
        controlCmd = GetFieldIDOrDie(env, incControl, "cmd", "Landroid/os/ParcelFileDescriptor;");
        controlPendingReads = GetFieldIDOrDie(env, incControl, "pendingReads",
                                              "Landroid/os/ParcelFileDescriptor;");
        controlLog = GetFieldIDOrDie(env, incControl, "log", "Landroid/os/ParcelFileDescriptor;");

        auto params = FindClassOrDie(env, "android/content/pm/DataLoaderParamsParcel");
        paramsType = GetFieldIDOrDie(env, params, "type", "I");
        paramsPackageName = GetFieldIDOrDie(env, params, "packageName", "Ljava/lang/String;");
        paramsClassName = GetFieldIDOrDie(env, params, "className", "Ljava/lang/String;");
        paramsArguments = GetFieldIDOrDie(env, params, "arguments", "Ljava/lang/String;");
        paramsDynamicArgs = GetFieldIDOrDie(env, params, "dynamicArgs",
                                            "[Landroid/content/pm/NamedParcelFileDescriptor;");

        auto namedFd = FindClassOrDie(env, "android/content/pm/NamedParcelFileDescriptor");
        namedFdName = GetFieldIDOrDie(env, namedFd, "name", "Ljava/lang/String;");
        namedFdFd = GetFieldIDOrDie(env, namedFd, "fd", "Landroid/os/ParcelFileDescriptor;");

        auto callbackControl =
                FindClassOrDie(env,
                               "android/content/pm/IPackageInstallerSessionFileSystemConnector");
        callbackControlWriteData =
                GetMethodIDOrDie(env, callbackControl, "writeData",
                                 "(Ljava/lang/String;JJLandroid/os/ParcelFileDescriptor;)V");
    }
};

const JniIds& jniIds(JNIEnv* env) {
    static const JniIds ids(env);
    return ids;
}

bool reportStatusViaCallback(JNIEnv* env, jobject listener, jint storageId, jint status) {
    if (listener == nullptr) {
        ALOGE("No listener object to talk to IncrementalService. "
              "DataLoaderId=%d, "
              "status=%d",
              storageId, status);
        return false;
    }

    const auto& jni = jniIds(env);

    env->CallVoidMethod(listener, jni.listenerOnStatusChanged, storageId, status);
    ALOGI("Reported status back to IncrementalService. DataLoaderId=%d, "
          "status=%d",
          storageId, status);

    return true;
}

class DataLoaderConnector;
using DataLoaderConnectorPtr = std::shared_ptr<DataLoaderConnector>;
using DataLoaderConnectorsMap = std::unordered_map<int, DataLoaderConnectorPtr>;

struct Globals {
    Globals() {
        dataLoaderFactory = new details::DataLoaderFactoryImpl(
                [](auto jvm) { return std::make_unique<ManagedDataLoader>(jvm); });
    }

    DataLoaderFactory managedDataLoaderFactory;
    DataLoaderFactory* dataLoaderFactory;

    std::mutex dataLoaderConnectorsLock;
    // id->DataLoader map
    DataLoaderConnectorsMap dataLoaderConnectors GUARDED_BY(dataLoaderConnectorsLock);

    std::atomic_bool stopped;
    std::thread cmdLooperThread;
    std::thread logLooperThread;
    std::vector<ReadInfo> pendingReads;
    std::vector<ReadInfo> pageReads;
};

static Globals& globals() {
    static Globals globals;
    return globals;
}

struct IncFsLooper : public android::Looper {
    IncFsLooper() : Looper(/*allowNonCallbacks=*/false) {}
    ~IncFsLooper() {}
};

static android::Looper& cmdLooper() {
    static IncFsLooper cmdLooper;
    return cmdLooper;
}

static android::Looper& logLooper() {
    static IncFsLooper logLooper;
    return logLooper;
}

struct DataLoaderParamsPair {
    static DataLoaderParamsPair createFromManaged(JNIEnv* env, jobject params);

    const android::dataloader::DataLoaderParams& dataLoaderParams() const {
        return mDataLoaderParams;
    }
    const ::DataLoaderParams& ndkDataLoaderParams() const { return mNDKDataLoaderParams; }

private:
    DataLoaderParamsPair(android::dataloader::DataLoaderParams&& dataLoaderParams);

    android::dataloader::DataLoaderParams mDataLoaderParams;
    ::DataLoaderParams mNDKDataLoaderParams;
    std::vector<DataLoaderNamedFd> mNamedFds;
};

static constexpr auto kPendingReadsBufferSize = 256;

class DataLoaderConnector : public FilesystemConnector, public StatusListener {
public:
    DataLoaderConnector(JNIEnv* env, jobject service, jint storageId, IncFsControl control,
                        jobject callbackControl, jobject listener)
          : mService(env->NewGlobalRef(service)),
            mCallbackControl(env->NewGlobalRef(callbackControl)),
            mListener(env->NewGlobalRef(listener)),
            mStorageId(storageId),
            mControl(control) {
        env->GetJavaVM(&mJvm);
        CHECK(mJvm != nullptr);
    }
    DataLoaderConnector(const DataLoaderConnector&) = delete;
    DataLoaderConnector(const DataLoaderConnector&&) = delete;
    virtual ~DataLoaderConnector() {
        JNIEnv* env = GetOrAttachJNIEnvironment(mJvm);

        env->DeleteGlobalRef(mService);
        env->DeleteGlobalRef(mCallbackControl);
        env->DeleteGlobalRef(mListener);

        close(mControl.cmd);
        close(mControl.logs);
    } // to avoid delete-non-virtual-dtor

    bool onCreate(DataLoaderFactory* factory, const DataLoaderParamsPair& params,
                  jobject managedParams) {
        mDataLoader = factory->onCreate(factory, &params.ndkDataLoaderParams(), this, this, mJvm,
                                        mService, managedParams);
        if (checkAndClearJavaException(__func__)) {
            return false;
        }
        if (!mDataLoader) {
            return false;
        }

        return true;
    }
    bool onStart() {
        CHECK(mDataLoader);
        bool result = mDataLoader->onStart(mDataLoader);
        if (checkAndClearJavaException(__func__)) {
            result = false;
        }
        return result;
    }
    void onStop() {
        CHECK(mDataLoader);
        mDataLoader->onStop(mDataLoader);
        checkAndClearJavaException(__func__);
    }
    void onDestroy() {
        CHECK(mDataLoader);
        mDataLoader->onDestroy(mDataLoader);
        checkAndClearJavaException(__func__);
    }

    bool onPrepareImage(jobject addedFiles, jobject removedFiles) {
        CHECK(mDataLoader);
        bool result = mDataLoader->onPrepareImage(mDataLoader, addedFiles, removedFiles);
        if (checkAndClearJavaException(__func__)) {
            result = false;
        }
        return result;
    }

    int onCmdLooperEvent(std::vector<ReadInfo>& pendingReads) {
        CHECK(mDataLoader);
        while (true) {
            pendingReads.resize(kPendingReadsBufferSize);
            if (android::incfs::waitForPendingReads(mControl, 0ms, &pendingReads) !=
                        android::incfs::WaitResult::HaveData ||
                pendingReads.empty()) {
                return 1;
            }
            mDataLoader->onPendingReads(mDataLoader, pendingReads.data(), pendingReads.size());
        }
        return 1;
    }
    int onLogLooperEvent(std::vector<ReadInfo>& pageReads) {
        CHECK(mDataLoader);
        while (true) {
            pageReads.clear();
            if (android::incfs::waitForPageReads(mControl, 0ms, &pageReads) !=
                        android::incfs::WaitResult::HaveData ||
                pageReads.empty()) {
                return 1;
            }
            mDataLoader->onPageReads(mDataLoader, pageReads.data(), pageReads.size());
        }
        return 1;
    }

    void writeData(jstring name, jlong offsetBytes, jlong lengthBytes, jobject incomingFd) const {
        CHECK(mDataLoader);
        JNIEnv* env = GetOrAttachJNIEnvironment(mJvm);
        const auto& jni = jniIds(env);
        return env->CallVoidMethod(mCallbackControl, jni.callbackControlWriteData, name,
                                   offsetBytes, lengthBytes, incomingFd);
    }

    int openWrite(FileId fid) const { return android::incfs::openWrite(mControl, fid).release(); }

    int writeBlocks(std::span<const IncFsDataBlock> blocks) const {
        return android::incfs::writeBlocks(blocks);
    }

    int getRawMetadata(FileId fid, char buffer[], size_t* bufferSize) const {
        return IncFs_GetMetadataById(mControl, fid, buffer, bufferSize);
    }

    bool reportStatus(DataLoaderStatus status) {
        if (status < DATA_LOADER_FIRST_STATUS || DATA_LOADER_LAST_STATUS < status) {
            ALOGE("Unable to report invalid status. status=%d", status);
            return false;
        }
        JNIEnv* env = GetOrAttachJNIEnvironment(mJvm);
        return reportStatusViaCallback(env, mListener, mStorageId, status);
    }

    bool checkAndClearJavaException(std::string_view method) {
        JNIEnv* env = GetOrAttachJNIEnvironment(mJvm);

        if (!env->ExceptionCheck()) {
            return false;
        }

        LOG(ERROR) << "Java exception during DataLoader::" << method;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }

    const IncFsControl& control() const { return mControl; }
    jobject listener() const { return mListener; }

private:
    JavaVM* mJvm = nullptr;
    jobject const mService;
    jobject const mCallbackControl;
    jobject const mListener;

    ::DataLoader* mDataLoader = nullptr;
    const jint mStorageId;
    const IncFsControl mControl;
};

static int onCmdLooperEvent(int fd, int events, void* data) {
    if (globals().stopped) {
        // No more listeners.
        return 0;
    }
    auto&& dataLoaderConnector = (DataLoaderConnector*)data;
    return dataLoaderConnector->onCmdLooperEvent(globals().pendingReads);
}

static int onLogLooperEvent(int fd, int events, void* data) {
    if (globals().stopped) {
        // No more listeners.
        return 0;
    }
    auto&& dataLoaderConnector = (DataLoaderConnector*)data;
    return dataLoaderConnector->onLogLooperEvent(globals().pageReads);
}

static int createFdFromManaged(JNIEnv* env, jobject pfd) {
    if (!pfd) {
        return -1;
    }

    const auto& jni = jniIds(env);
    auto managedFd = env->CallObjectMethod(pfd, jni.parcelFileDescriptorGetFileDescriptor);
    return dup(jniGetFDFromFileDescriptor(env, managedFd));
}

static jobject createCallbackControl(JNIEnv* env, jobject managedControl) {
    const auto& jni = jniIds(env);
    return env->GetObjectField(managedControl, jni.callback);
}

static IncFsControl createIncFsControlFromManaged(JNIEnv* env, jobject managedControl) {
    const auto& jni = jniIds(env);
    auto managedIncControl = env->GetObjectField(managedControl, jni.incremental);
    if (!managedIncControl) {
        return {-1, -1, -1};
    }
    auto cmd = createFdFromManaged(env, env->GetObjectField(managedIncControl, jni.controlCmd));
    auto pr = createFdFromManaged(env,
                                  env->GetObjectField(managedIncControl, jni.controlPendingReads));
    auto log = createFdFromManaged(env, env->GetObjectField(managedIncControl, jni.controlLog));
    return {cmd, pr, log};
}

DataLoaderParamsPair::DataLoaderParamsPair(android::dataloader::DataLoaderParams&& dataLoaderParams)
      : mDataLoaderParams(std::move(dataLoaderParams)) {
    mNDKDataLoaderParams.type = mDataLoaderParams.type();
    mNDKDataLoaderParams.packageName = mDataLoaderParams.packageName().c_str();
    mNDKDataLoaderParams.className = mDataLoaderParams.className().c_str();
    mNDKDataLoaderParams.arguments = mDataLoaderParams.arguments().c_str();

    mNamedFds.resize(mDataLoaderParams.dynamicArgs().size());
    for (size_t i = 0, size = mNamedFds.size(); i < size; ++i) {
        const auto& arg = mDataLoaderParams.dynamicArgs()[i];
        mNamedFds[i].name = arg.name.c_str();
        mNamedFds[i].fd = arg.fd;
    }
    mNDKDataLoaderParams.dynamicArgsSize = mNamedFds.size();
    mNDKDataLoaderParams.dynamicArgs = mNamedFds.data();
}

DataLoaderParamsPair DataLoaderParamsPair::createFromManaged(JNIEnv* env, jobject managedParams) {
    const auto& jni = jniIds(env);

    const int type = env->GetIntField(managedParams, jni.paramsType);

    std::string packageName(
            env->GetStringUTFChars((jstring)env->GetObjectField(managedParams,
                                                                jni.paramsPackageName),
                                   nullptr));
    std::string className(
            env->GetStringUTFChars((jstring)env->GetObjectField(managedParams, jni.paramsClassName),
                                   nullptr));
    std::string arguments(
            env->GetStringUTFChars((jstring)env->GetObjectField(managedParams, jni.paramsArguments),
                                   nullptr));

    auto dynamicArgsArray = (jobjectArray)env->GetObjectField(managedParams, jni.paramsDynamicArgs);

    size_t size = env->GetArrayLength(dynamicArgsArray);
    std::vector<android::dataloader::DataLoaderParams::NamedFd> dynamicArgs(size);
    for (size_t i = 0; i < size; ++i) {
        auto dynamicArg = env->GetObjectArrayElement(dynamicArgsArray, i);
        auto pfd = env->GetObjectField(dynamicArg, jni.namedFdFd);
        auto fd = env->CallObjectMethod(pfd, jni.parcelFileDescriptorGetFileDescriptor);
        dynamicArgs[i].fd = jniGetFDFromFileDescriptor(env, fd);
        dynamicArgs[i].name =
                (env->GetStringUTFChars((jstring)env->GetObjectField(dynamicArg, jni.namedFdName),
                                        nullptr));
    }

    return DataLoaderParamsPair(android::dataloader::DataLoaderParams(type, std::move(packageName),
                                                                      std::move(className),
                                                                      std::move(arguments),
                                                                      std::move(dynamicArgs)));
}

static void cmdLooperThread() {
    constexpr auto kTimeoutMsecs = 60 * 1000;
    while (!globals().stopped) {
        cmdLooper().pollAll(kTimeoutMsecs);
    }
}

static void logLooperThread() {
    constexpr auto kTimeoutMsecs = 60 * 1000;
    while (!globals().stopped) {
        logLooper().pollAll(kTimeoutMsecs);
    }
}

static std::string pathFromFd(int fd) {
    static constexpr char fdNameFormat[] = "/proc/self/fd/%d";
    char fdNameBuffer[NELEM(fdNameFormat) + 11 + 1]; // max int length + '\0'
    snprintf(fdNameBuffer, NELEM(fdNameBuffer), fdNameFormat, fd);

    std::string res;
    // lstat() is supposed to return us exactly the needed buffer size, but
    // somehow it may also return a smaller (but still >0) st_size field.
    // That's why let's only use it for the initial estimate.
    struct stat st = {};
    if (::lstat(fdNameBuffer, &st) || st.st_size == 0) {
        st.st_size = PATH_MAX;
    }
    auto bufSize = st.st_size;
    for (;;) {
        res.resize(bufSize + 1, '\0');
        auto size = ::readlink(fdNameBuffer, &res[0], res.size());
        if (size < 0) {
            return {};
        }
        if (size > bufSize) {
            // File got renamed in between lstat() and readlink() calls? Retry.
            bufSize *= 2;
            continue;
        }
        res.resize(size);
        return res;
    }
}

} // namespace

void DataLoader_Initialize(struct ::DataLoaderFactory* factory) {
    CHECK(factory) << "DataLoader factory is invalid.";
    globals().dataLoaderFactory = factory;
}

void DataLoader_FilesystemConnector_writeData(DataLoaderFilesystemConnectorPtr ifs, jstring name,
                                              jlong offsetBytes, jlong lengthBytes,
                                              jobject incomingFd) {
    auto connector = static_cast<DataLoaderConnector*>(ifs);
    return connector->writeData(name, offsetBytes, lengthBytes, incomingFd);
}

int DataLoader_FilesystemConnector_openWrite(DataLoaderFilesystemConnectorPtr ifs,
                                             IncFsFileId fid) {
    auto connector = static_cast<DataLoaderConnector*>(ifs);
    return connector->openWrite(fid);
}

int DataLoader_FilesystemConnector_writeBlocks(DataLoaderFilesystemConnectorPtr ifs,
                                               const IncFsDataBlock blocks[], int blocksCount) {
    auto connector = static_cast<DataLoaderConnector*>(ifs);
    return connector->writeBlocks({blocks, blocksCount});
}

int DataLoader_FilesystemConnector_getRawMetadata(DataLoaderFilesystemConnectorPtr ifs,
                                                  IncFsFileId fid, char buffer[],
                                                  size_t* bufferSize) {
    auto connector = static_cast<DataLoaderConnector*>(ifs);
    return connector->getRawMetadata(fid, buffer, bufferSize);
}

int DataLoader_StatusListener_reportStatus(DataLoaderStatusListenerPtr listener,
                                           DataLoaderStatus status) {
    auto connector = static_cast<DataLoaderConnector*>(listener);
    return connector->reportStatus(status);
}

bool DataLoaderService_OnCreate(JNIEnv* env, jobject service, jint storageId, jobject control,
                                jobject params, jobject listener) {
    auto reportDestroyed = [env, storageId](jobject listener) {
        if (listener) {
            return;
        }
        const auto& jni = jniIds(env);
        reportStatusViaCallback(env, listener, storageId, jni.constants.DATA_LOADER_DESTROYED);
    };
    std::unique_ptr<_jobject, decltype(reportDestroyed)> reportDestroyedOnExit(listener,
                                                                               reportDestroyed);

    auto nativeControl = createIncFsControlFromManaged(env, control);
    ALOGE("DataLoader::create1 cmd: %d/%s", nativeControl.cmd,
          pathFromFd(nativeControl.cmd).c_str());
    ALOGE("DataLoader::create1 log: %d/%s", nativeControl.logs,
          pathFromFd(nativeControl.logs).c_str());

    auto nativeParams = DataLoaderParamsPair::createFromManaged(env, params);
    ALOGE("DataLoader::create2: %d/%s/%s/%s/%d", nativeParams.dataLoaderParams().type(),
          nativeParams.dataLoaderParams().packageName().c_str(),
          nativeParams.dataLoaderParams().className().c_str(),
          nativeParams.dataLoaderParams().arguments().c_str(),
          (int)nativeParams.dataLoaderParams().dynamicArgs().size());

    auto callbackControl = createCallbackControl(env, control);

    CHECK(globals().dataLoaderFactory) << "Unable to create DataLoader: factory is missing.";
    auto dataLoaderConnector =
            std::make_unique<DataLoaderConnector>(env, service, storageId, nativeControl,
                                                  callbackControl, listener);
    {
        std::lock_guard lock{globals().dataLoaderConnectorsLock};
        auto [dlIt, dlInserted] =
                globals().dataLoaderConnectors.try_emplace(storageId,
                                                           std::move(dataLoaderConnector));
        if (!dlInserted) {
            ALOGE("Failed to insert id(%d)->DataLoader mapping, fd already "
                  "exists",
                  storageId);
            return false;
        }
        if (!dlIt->second->onCreate(globals().dataLoaderFactory, nativeParams, params)) {
            globals().dataLoaderConnectors.erase(dlIt);
            return false;
        }
    }

    reportDestroyedOnExit.release();

    const auto& jni = jniIds(env);
    reportStatusViaCallback(env, listener, storageId, jni.constants.DATA_LOADER_CREATED);

    return true;
}

bool DataLoaderService_OnStart(JNIEnv* env, jint storageId) {
    auto reportStopped = [env, storageId](jobject listener) {
        if (listener) {
            return;
        }
        const auto& jni = jniIds(env);
        reportStatusViaCallback(env, listener, storageId, jni.constants.DATA_LOADER_STOPPED);
    };
    std::unique_ptr<_jobject, decltype(reportStopped)> reportStoppedOnExit(nullptr, reportStopped);

    IncFsControl control;
    jobject listener;
    DataLoaderConnectorPtr dataLoaderConnector;
    {
        std::lock_guard lock{globals().dataLoaderConnectorsLock};
        auto dlIt = globals().dataLoaderConnectors.find(storageId);
        if (dlIt == globals().dataLoaderConnectors.end()) {
            ALOGE("Failed to start id(%d): not found", storageId);
            return false;
        }

        listener = dlIt->second->listener();
        reportStoppedOnExit.reset(listener);

        dataLoaderConnector = dlIt->second;
        if (!dataLoaderConnector->onStart()) {
            ALOGE("Failed to start id(%d): onStart returned false", storageId);
            return false;
        }

        control = dataLoaderConnector->control();

        // Create loopers while we are under lock.
        if (control.cmd >= 0 && !globals().cmdLooperThread.joinable()) {
            cmdLooper();
            globals().cmdLooperThread = std::thread(&cmdLooperThread);
        }
        if (control.logs >= 0 && !globals().logLooperThread.joinable()) {
            logLooper();
            globals().logLooperThread = std::thread(&logLooperThread);
        }
    }

    if (control.cmd >= 0) {
        cmdLooper().addFd(control.cmd, android::Looper::POLL_CALLBACK, android::Looper::EVENT_INPUT,
                          &onCmdLooperEvent, dataLoaderConnector.get());
        cmdLooper().wake();
    }

    if (control.logs >= 0) {
        logLooper().addFd(control.logs, android::Looper::POLL_CALLBACK,
                          android::Looper::EVENT_INPUT, &onLogLooperEvent,
                          dataLoaderConnector.get());
        logLooper().wake();
    }

    reportStoppedOnExit.release();

    const auto& jni = jniIds(env);
    reportStatusViaCallback(env, listener, storageId, jni.constants.DATA_LOADER_STARTED);

    return true;
}

bool DataLoaderService_OnStop(JNIEnv* env, jint storageId) {
    auto reportStopped = [env, storageId](jobject listener) {
        if (listener) {
            return;
        }
        const auto& jni = jniIds(env);
        reportStatusViaCallback(env, listener, storageId, jni.constants.DATA_LOADER_STOPPED);
    };
    std::unique_ptr<_jobject, decltype(reportStopped)> reportStoppedOnExit(nullptr, reportStopped);

    IncFsControl control;
    {
        std::lock_guard lock{globals().dataLoaderConnectorsLock};
        auto dlIt = globals().dataLoaderConnectors.find(storageId);
        if (dlIt == globals().dataLoaderConnectors.end()) {
            ALOGE("Failed to stop id(%d): not found", storageId);
            return false;
        }
        control = dlIt->second->control();

        reportStoppedOnExit.reset(dlIt->second->listener());
    }

    if (control.cmd >= 0) {
        cmdLooper().removeFd(control.cmd);
        cmdLooper().wake();
    }
    if (control.logs >= 0) {
        logLooper().removeFd(control.logs);
        logLooper().wake();
    }

    {
        std::lock_guard lock{globals().dataLoaderConnectorsLock};
        auto dlIt = globals().dataLoaderConnectors.find(storageId);
        if (dlIt == globals().dataLoaderConnectors.end()) {
            ALOGE("Failed to stop id(%d): not found", storageId);
            return false;
        }
        auto&& dataLoaderConnector = dlIt->second;
        if (dataLoaderConnector) {
            dataLoaderConnector->onStop();
        }
    }

    return true;
}

bool DataLoaderService_OnDestroy(JNIEnv* env, jint storageId) {
    DataLoaderService_OnStop(env, storageId);

    auto reportDestroyed = [env, storageId](jobject listener) {
        if (listener) {
            return;
        }
        const auto& jni = jniIds(env);
        reportStatusViaCallback(env, listener, storageId, jni.constants.DATA_LOADER_DESTROYED);
    };
    std::unique_ptr<_jobject, decltype(reportDestroyed)> reportDestroyedOnExit(nullptr,
                                                                               reportDestroyed);

    std::lock_guard lock{globals().dataLoaderConnectorsLock};
    auto dlIt = globals().dataLoaderConnectors.find(storageId);
    if (dlIt == globals().dataLoaderConnectors.end()) {
        ALOGE("Failed to remove id(%d): not found", storageId);
        return false;
    }
    reportDestroyedOnExit.reset(env->NewLocalRef(dlIt->second->listener()));

    auto&& dataLoaderConnector = dlIt->second;
    dataLoaderConnector->onDestroy();
    globals().dataLoaderConnectors.erase(dlIt);

    return true;
}

bool DataLoaderService_OnPrepareImage(JNIEnv* env, jint storageId, jobject addedFiles,
                                      jobject removedFiles) {
    jobject listener;
    bool result;
    {
        std::lock_guard lock{globals().dataLoaderConnectorsLock};
        auto dlIt = globals().dataLoaderConnectors.find(storageId);
        if (dlIt == globals().dataLoaderConnectors.end()) {
            ALOGE("Failed to handle onPrepareImage for id(%d): not found", storageId);
            return false;
        }
        listener = dlIt->second->listener();

        auto&& dataLoaderConnector = dlIt->second;
        result = dataLoaderConnector->onPrepareImage(addedFiles, removedFiles);
    }

    const auto& jni = jniIds(env);
    reportStatusViaCallback(env, listener, storageId,
                            result ? jni.constants.DATA_LOADER_IMAGE_READY
                                   : jni.constants.DATA_LOADER_IMAGE_NOT_READY);

    return result;
}
