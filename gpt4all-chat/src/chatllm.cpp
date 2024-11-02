#include "chatllm.h"

#include "chat.h"
#include "chatapi.h"
#include "chatmodel.h"
#include "jinja_helpers.h"
#include "localdocs.h"
#include "mysettings.h"
#include "network.h"

#include <fmt/format.h>

// FIXME(jared): Jinja2Cpp headers should compile with -Werror=undef
#ifdef __GNUC__
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wundef"
#endif
#include <jinja2cpp/template_env.h>
#ifdef __GNUC__
#   pragma GCC diagnostic pop
#endif

#include <QDataStream>
#include <QDebug>
#include <QFile>
#include <QGlobalStatic>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QWaitCondition>
#include <Qt>
#include <QtLogging>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace Qt::Literals::StringLiterals;
namespace ranges = std::ranges;
namespace views = std::views;

//#define DEBUG
//#define DEBUG_MODEL_LOADING

// NOTE: not threadsafe
static jinja2::TemplateEnv *jinjaEnv()
{
    static std::optional<jinja2::TemplateEnv> env;
    if (!env) {
        auto &e = env.emplace();
        auto &settings = e.GetSettings();
        settings.trimBlocks = true;
        settings.lstripBlocks = true;
    }
    return &*env;
}

class LLModelStore {
public:
    static LLModelStore *globalInstance();

    LLModelInfo acquireModel(); // will block until llmodel is ready
    void releaseModel(LLModelInfo &&info); // must be called when you are done
    void destroy();

private:
    LLModelStore()
    {
        // seed with empty model
        m_availableModel = LLModelInfo();
    }
    ~LLModelStore() {}
    std::optional<LLModelInfo> m_availableModel;
    QMutex m_mutex;
    QWaitCondition m_condition;
    friend class MyLLModelStore;
};

class MyLLModelStore : public LLModelStore { };
Q_GLOBAL_STATIC(MyLLModelStore, storeInstance)
LLModelStore *LLModelStore::globalInstance()
{
    return storeInstance();
}

LLModelInfo LLModelStore::acquireModel()
{
    QMutexLocker locker(&m_mutex);
    while (!m_availableModel)
        m_condition.wait(locker.mutex());
    auto first = std::move(*m_availableModel);
    m_availableModel.reset();
    return first;
}

void LLModelStore::releaseModel(LLModelInfo &&info)
{
    QMutexLocker locker(&m_mutex);
    Q_ASSERT(!m_availableModel);
    m_availableModel = std::move(info);
    m_condition.wakeAll();
}

void LLModelStore::destroy()
{
    QMutexLocker locker(&m_mutex);
    m_availableModel.reset();
}

void LLModelInfo::resetModel(ChatLLM *cllm, LLModel *model) {
    this->model.reset(model);
    fallbackReason.reset();
    emit cllm->loadedModelInfoChanged();
}

ChatLLM::ChatLLM(Chat *parent, bool isServer)
    : QObject{nullptr}
    , m_chat(parent)
    , m_shouldBeLoaded(false)
    , m_forceUnloadModel(false)
    , m_markedForDeletion(false)
    , m_stopGenerating(false)
    , m_timer(nullptr)
    , m_isServer(isServer)
    , m_forceMetal(MySettings::globalInstance()->forceMetal())
    , m_reloadingToChangeVariant(false)
    , m_chatModel(parent->chatModel())
{
    moveToThread(&m_llmThread);
    connect(this, &ChatLLM::shouldBeLoadedChanged, this, &ChatLLM::handleShouldBeLoadedChanged,
        Qt::QueuedConnection); // explicitly queued
    connect(this, &ChatLLM::trySwitchContextRequested, this, &ChatLLM::trySwitchContextOfLoadedModel,
        Qt::QueuedConnection); // explicitly queued
    connect(parent, &Chat::idChanged, this, &ChatLLM::handleChatIdChanged);
    connect(&m_llmThread, &QThread::started, this, &ChatLLM::handleThreadStarted);
    connect(MySettings::globalInstance(), &MySettings::forceMetalChanged, this, &ChatLLM::handleForceMetalChanged);
    connect(MySettings::globalInstance(), &MySettings::deviceChanged, this, &ChatLLM::handleDeviceChanged);

    // The following are blocking operations and will block the llm thread
    connect(this, &ChatLLM::requestRetrieveFromDB, LocalDocs::globalInstance()->database(), &Database::retrieveFromDB,
        Qt::BlockingQueuedConnection);

    m_llmThread.setObjectName(parent->id());
    m_llmThread.start();
}

ChatLLM::~ChatLLM()
{
    destroy();
}

void ChatLLM::destroy()
{
    m_stopGenerating = true;
    m_llmThread.quit();
    m_llmThread.wait();

    // The only time we should have a model loaded here is on shutdown
    // as we explicitly unload the model in all other circumstances
    if (isModelLoaded()) {
        m_llModelInfo.resetModel(this);
    }
}

void ChatLLM::destroyStore()
{
    LLModelStore::globalInstance()->destroy();
}

void ChatLLM::handleThreadStarted()
{
    m_timer = new TokenTimer(this);
    connect(m_timer, &TokenTimer::report, this, &ChatLLM::reportSpeed);
    emit threadStarted();
}

void ChatLLM::handleForceMetalChanged(bool forceMetal)
{
#if defined(Q_OS_MAC) && defined(__aarch64__)
    m_forceMetal = forceMetal;
    if (isModelLoaded() && m_shouldBeLoaded) {
        m_reloadingToChangeVariant = true;
        unloadModel();
        reloadModel();
        m_reloadingToChangeVariant = false;
    }
#else
    Q_UNUSED(forceMetal);
#endif
}

void ChatLLM::handleDeviceChanged()
{
    if (isModelLoaded() && m_shouldBeLoaded) {
        m_reloadingToChangeVariant = true;
        unloadModel();
        reloadModel();
        m_reloadingToChangeVariant = false;
    }
}

bool ChatLLM::loadDefaultModel()
{
    ModelInfo defaultModel = ModelList::globalInstance()->defaultModelInfo();
    if (defaultModel.filename().isEmpty()) {
        emit modelLoadingError(u"Could not find any model to load"_s);
        return false;
    }
    return loadModel(defaultModel);
}

void ChatLLM::trySwitchContextOfLoadedModel(const ModelInfo &modelInfo)
{
    // We're trying to see if the store already has the model fully loaded that we wish to use
    // and if so we just acquire it from the store and switch the context and return true. If the
    // store doesn't have it or we're already loaded or in any other case just return false.

    // If we're already loaded or a server or we're reloading to change the variant/device or the
    // modelInfo is empty, then this should fail
    if (
        isModelLoaded() || m_isServer || m_reloadingToChangeVariant || modelInfo.name().isEmpty() || !m_shouldBeLoaded
    ) {
        emit trySwitchContextOfLoadedModelCompleted(0);
        return;
    }

    QString filePath = modelInfo.dirpath + modelInfo.filename();
    QFileInfo fileInfo(filePath);

    acquireModel();
#if defined(DEBUG_MODEL_LOADING)
        qDebug() << "acquired model from store" << m_llmThread.objectName() << m_llModelInfo.model.get();
#endif

    // The store gave us no already loaded model, the wrong type of model, then give it back to the
    // store and fail
    if (!m_llModelInfo.model || m_llModelInfo.fileInfo != fileInfo || !m_shouldBeLoaded) {
        LLModelStore::globalInstance()->releaseModel(std::move(m_llModelInfo));
        emit trySwitchContextOfLoadedModelCompleted(0);
        return;
    }

#if defined(DEBUG_MODEL_LOADING)
    qDebug() << "store had our model" << m_llmThread.objectName() << m_llModelInfo.model.get();
#endif

    emit trySwitchContextOfLoadedModelCompleted(2);
    emit modelLoadingPercentageChanged(1.0f);
    emit trySwitchContextOfLoadedModelCompleted(0);
}

bool ChatLLM::loadModel(const ModelInfo &modelInfo)
{
    // This is a complicated method because N different possible threads are interested in the outcome
    // of this method. Why? Because we have a main/gui thread trying to monitor the state of N different
    // possible chat threads all vying for a single resource - the currently loaded model - as the user
    // switches back and forth between chats. It is important for our main/gui thread to never block
    // but simultaneously always have up2date information with regards to which chat has the model loaded
    // and what the type and name of that model is. I've tried to comment extensively in this method
    // to provide an overview of what we're doing here.

    if (isModelLoaded() && this->modelInfo() == modelInfo) {
        // already acquired -> keep it
        return true; // already loaded
    }

    // reset status
    emit modelLoadingPercentageChanged(std::numeric_limits<float>::min()); // small non-zero positive value
    emit modelLoadingError("");

    QString filePath = modelInfo.dirpath + modelInfo.filename();
    QFileInfo fileInfo(filePath);

    // We have a live model, but it isn't the one we want
    bool alreadyAcquired = isModelLoaded();
    if (alreadyAcquired) {
#if defined(DEBUG_MODEL_LOADING)
        qDebug() << "already acquired model deleted" << m_llmThread.objectName() << m_llModelInfo.model.get();
#endif
        m_llModelInfo.resetModel(this);
    } else if (!m_isServer) {
        // This is a blocking call that tries to retrieve the model we need from the model store.
        // If it succeeds, then we just have to restore state. If the store has never had a model
        // returned to it, then the modelInfo.model pointer should be null which will happen on startup
        acquireModel();
#if defined(DEBUG_MODEL_LOADING)
        qDebug() << "acquired model from store" << m_llmThread.objectName() << m_llModelInfo.model.get();
#endif
        // At this point it is possible that while we were blocked waiting to acquire the model from the
        // store, that our state was changed to not be loaded. If this is the case, release the model
        // back into the store and quit loading
        if (!m_shouldBeLoaded) {
#if defined(DEBUG_MODEL_LOADING)
            qDebug() << "no longer need model" << m_llmThread.objectName() << m_llModelInfo.model.get();
#endif
            LLModelStore::globalInstance()->releaseModel(std::move(m_llModelInfo));
            emit modelLoadingPercentageChanged(0.0f);
            return false;
        }

        // Check if the store just gave us exactly the model we were looking for
        if (m_llModelInfo.model && m_llModelInfo.fileInfo == fileInfo && !m_reloadingToChangeVariant) {
#if defined(DEBUG_MODEL_LOADING)
            qDebug() << "store had our model" << m_llmThread.objectName() << m_llModelInfo.model.get();
#endif
            emit modelLoadingPercentageChanged(1.0f);
            setModelInfo(modelInfo);
            Q_ASSERT(!m_modelInfo.filename().isEmpty());
            if (m_modelInfo.filename().isEmpty())
                emit modelLoadingError(u"Modelinfo is left null for %1"_s.arg(modelInfo.filename()));
            return true;
        } else {
            // Release the memory since we have to switch to a different model.
#if defined(DEBUG_MODEL_LOADING)
            qDebug() << "deleting model" << m_llmThread.objectName() << m_llModelInfo.model.get();
#endif
            m_llModelInfo.resetModel(this);
        }
    }

    // Guarantee we've released the previous models memory
    Q_ASSERT(!m_llModelInfo.model);

    // Store the file info in the modelInfo in case we have an error loading
    m_llModelInfo.fileInfo = fileInfo;

    if (fileInfo.exists()) {
        QVariantMap modelLoadProps;
        if (modelInfo.isOnline) {
            QString apiKey;
            QString requestUrl;
            QString modelName;
            {
                QFile file(filePath);
                bool success = file.open(QIODeviceBase::ReadOnly);
                (void)success;
                Q_ASSERT(success);
                QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
                QJsonObject obj = doc.object();
                apiKey = obj["apiKey"].toString();
                modelName = obj["modelName"].toString();
                if (modelInfo.isCompatibleApi) {
                    QString baseUrl(obj["baseUrl"].toString());
                    QUrl apiUrl(QUrl::fromUserInput(baseUrl));
                    if (!Network::isHttpUrlValid(apiUrl)) {
                        return false;
                    }
                    QString currentPath(apiUrl.path());
                    QString suffixPath("%1/chat/completions");
                    apiUrl.setPath(suffixPath.arg(currentPath));
                    requestUrl = apiUrl.toString();
                } else {
                    requestUrl = modelInfo.url();
                }
            }
            m_llModelType = LLModelTypeV1::API;
            ChatAPI *model = new ChatAPI();
            model->setModelName(modelName);
            model->setRequestURL(requestUrl);
            model->setAPIKey(apiKey);
            m_llModelInfo.resetModel(this, model);
        } else if (!loadNewModel(modelInfo, modelLoadProps)) {
            return false; // m_shouldBeLoaded became false
        }
#if defined(DEBUG_MODEL_LOADING)
        qDebug() << "new model" << m_llmThread.objectName() << m_llModelInfo.model.get();
#endif
#if defined(DEBUG)
        qDebug() << "modelLoadedChanged" << m_llmThread.objectName();
        fflush(stdout);
#endif
        emit modelLoadingPercentageChanged(isModelLoaded() ? 1.0f : 0.0f);
        emit loadedModelInfoChanged();

        modelLoadProps.insert("requestedDevice", MySettings::globalInstance()->device());
        modelLoadProps.insert("model", modelInfo.filename());
        Network::globalInstance()->trackChatEvent("model_load", modelLoadProps);
    } else {
        if (!m_isServer)
            LLModelStore::globalInstance()->releaseModel(std::move(m_llModelInfo)); // release back into the store
        resetModel();
        emit modelLoadingError(u"Could not find file for model %1"_s.arg(modelInfo.filename()));
    }

    if (m_llModelInfo.model)
        setModelInfo(modelInfo);
    return bool(m_llModelInfo.model);
}

/* Returns false if the model should no longer be loaded (!m_shouldBeLoaded).
 * Otherwise returns true, even on error. */
bool ChatLLM::loadNewModel(const ModelInfo &modelInfo, QVariantMap &modelLoadProps)
{
    QElapsedTimer modelLoadTimer;
    modelLoadTimer.start();

    QString requestedDevice = MySettings::globalInstance()->device();
    int n_ctx = MySettings::globalInstance()->modelContextLength(modelInfo);
    int ngl = MySettings::globalInstance()->modelGpuLayers(modelInfo);

    std::string backend = "auto";
#ifdef Q_OS_MAC
    if (requestedDevice == "CPU") {
        backend = "cpu";
    } else if (m_forceMetal) {
#ifdef __aarch64__
        backend = "metal";
#endif
    }
#else // !defined(Q_OS_MAC)
    if (requestedDevice.startsWith("CUDA: "))
        backend = "cuda";
#endif

    QString filePath = modelInfo.dirpath + modelInfo.filename();

    auto construct = [this, &filePath, &modelInfo, &modelLoadProps, n_ctx](std::string const &backend) {
        QString constructError;
        m_llModelInfo.resetModel(this);
        try {
            auto *model = LLModel::Implementation::construct(filePath.toStdString(), backend, n_ctx);
            m_llModelInfo.resetModel(this, model);
        } catch (const LLModel::MissingImplementationError &e) {
            modelLoadProps.insert("error", "missing_model_impl");
            constructError = e.what();
        } catch (const LLModel::UnsupportedModelError &e) {
            modelLoadProps.insert("error", "unsupported_model_file");
            constructError = e.what();
        } catch (const LLModel::BadArchError &e) {
            constructError = e.what();
            modelLoadProps.insert("error", "unsupported_model_arch");
            modelLoadProps.insert("model_arch", QString::fromStdString(e.arch()));
        }

        if (!m_llModelInfo.model) {
            if (!m_isServer)
                LLModelStore::globalInstance()->releaseModel(std::move(m_llModelInfo));
            resetModel();
            emit modelLoadingError(u"Error loading %1: %2"_s.arg(modelInfo.filename(), constructError));
            return false;
        }

        m_llModelInfo.model->setProgressCallback([this](float progress) -> bool {
            progress = std::max(progress, std::numeric_limits<float>::min()); // keep progress above zero
            emit modelLoadingPercentageChanged(progress);
            return m_shouldBeLoaded;
        });
        return true;
    };

    if (!construct(backend))
        return true;

    if (m_llModelInfo.model->isModelBlacklisted(filePath.toStdString())) {
        static QSet<QString> warned;
        auto fname = modelInfo.filename();
        if (!warned.contains(fname)) {
            emit modelLoadingWarning(
                u"%1 is known to be broken. Please get a replacement via the download dialog."_s.arg(fname)
            );
            warned.insert(fname); // don't warn again until restart
        }
    }

    auto approxDeviceMemGB = [](const LLModel::GPUDevice *dev) {
        float memGB = dev->heapSize / float(1024 * 1024 * 1024);
        return std::floor(memGB * 10.f) / 10.f; // truncate to 1 decimal place
    };

    std::vector<LLModel::GPUDevice> availableDevices;
    const LLModel::GPUDevice *defaultDevice = nullptr;
    {
        const size_t requiredMemory = m_llModelInfo.model->requiredMem(filePath.toStdString(), n_ctx, ngl);
        availableDevices = m_llModelInfo.model->availableGPUDevices(requiredMemory);
        // Pick the best device
        // NB: relies on the fact that Kompute devices are listed first
        if (!availableDevices.empty() && availableDevices.front().type == 2 /*a discrete gpu*/) {
            defaultDevice = &availableDevices.front();
            float memGB = defaultDevice->heapSize / float(1024 * 1024 * 1024);
            memGB = std::floor(memGB * 10.f) / 10.f; // truncate to 1 decimal place
            modelLoadProps.insert("default_device", QString::fromStdString(defaultDevice->name));
            modelLoadProps.insert("default_device_mem", approxDeviceMemGB(defaultDevice));
            modelLoadProps.insert("default_device_backend", QString::fromStdString(defaultDevice->backendName()));
        }
    }

    bool actualDeviceIsCPU = true;

#if defined(Q_OS_MAC) && defined(__aarch64__)
    if (m_llModelInfo.model->implementation().buildVariant() == "metal")
        actualDeviceIsCPU = false;
#else
    if (requestedDevice != "CPU") {
        const auto *device = defaultDevice;
        if (requestedDevice != "Auto") {
            // Use the selected device
            for (const LLModel::GPUDevice &d : availableDevices) {
                if (QString::fromStdString(d.selectionName()) == requestedDevice) {
                    device = &d;
                    break;
                }
            }
        }

        std::string unavail_reason;
        if (!device) {
            // GPU not available
        } else if (!m_llModelInfo.model->initializeGPUDevice(device->index, &unavail_reason)) {
            m_llModelInfo.fallbackReason = QString::fromStdString(unavail_reason);
        } else {
            actualDeviceIsCPU = false;
            modelLoadProps.insert("requested_device_mem", approxDeviceMemGB(device));
        }
    }
#endif

    bool success = m_llModelInfo.model->loadModel(filePath.toStdString(), n_ctx, ngl);

    if (!m_shouldBeLoaded) {
        m_llModelInfo.resetModel(this);
        if (!m_isServer)
            LLModelStore::globalInstance()->releaseModel(std::move(m_llModelInfo));
        resetModel();
        emit modelLoadingPercentageChanged(0.0f);
        return false;
    }

    if (actualDeviceIsCPU) {
        // we asked llama.cpp to use the CPU
    } else if (!success) {
        // llama_init_from_file returned nullptr
        m_llModelInfo.fallbackReason = "GPU loading failed (out of VRAM?)";
        modelLoadProps.insert("cpu_fallback_reason", "gpu_load_failed");

        // For CUDA, make sure we don't use the GPU at all - ngl=0 still offloads matmuls
        if (backend == "cuda" && !construct("auto"))
            return true;

        success = m_llModelInfo.model->loadModel(filePath.toStdString(), n_ctx, 0);

        if (!m_shouldBeLoaded) {
            m_llModelInfo.resetModel(this);
            if (!m_isServer)
                LLModelStore::globalInstance()->releaseModel(std::move(m_llModelInfo));
            resetModel();
            emit modelLoadingPercentageChanged(0.0f);
            return false;
        }
    } else if (!m_llModelInfo.model->usingGPUDevice()) {
        // ggml_vk_init was not called in llama.cpp
        // We might have had to fallback to CPU after load if the model is not possible to accelerate
        // for instance if the quantization method is not supported on Vulkan yet
        m_llModelInfo.fallbackReason = "model or quant has no GPU support";
        modelLoadProps.insert("cpu_fallback_reason", "gpu_unsupported_model");
    }

    if (!success) {
        m_llModelInfo.resetModel(this);
        if (!m_isServer)
            LLModelStore::globalInstance()->releaseModel(std::move(m_llModelInfo));
        resetModel();
        emit modelLoadingError(u"Could not load model due to invalid model file for %1"_s.arg(modelInfo.filename()));
        modelLoadProps.insert("error", "loadmodel_failed");
        return true;
    }

    switch (m_llModelInfo.model->implementation().modelType()[0]) {
    case 'L': m_llModelType = LLModelTypeV1::LLAMA; break;
    default:
        {
            m_llModelInfo.resetModel(this);
            if (!m_isServer)
                LLModelStore::globalInstance()->releaseModel(std::move(m_llModelInfo));
            resetModel();
            emit modelLoadingError(u"Could not determine model type for %1"_s.arg(modelInfo.filename()));
        }
    }

    modelLoadProps.insert("$duration", modelLoadTimer.elapsed() / 1000.);
    return true;
}

bool ChatLLM::isModelLoaded() const
{
    return m_llModelInfo.model && m_llModelInfo.model->isModelLoaded();
}

QString &removeLeadingWhitespace(QString &s)
{
    auto firstNonSpace = ranges::find_if_not(s, [](auto c) { return c.isSpace(); });
    s.remove(0, firstNonSpace - s.begin());
    return s;
}

// FIXME(jared): we don't actually have to re-decode the prompt to generate a new response
void ChatLLM::regenerateResponse()
{
    // TODO(jared): implement this
#if 0
    auto &chatModel = *m_chatModel;
    int totalItems = chatModel.count();
    if (totalItems < 2)
        return;

    int responseIndex = totalItems - 1;
    auto responseItem = chatModel.get(responseIndex);
    if (responseItem.name != "Response: ")
        return;

    m_ctx.n_past -= m_promptResponseTokens;
    m_promptResponseTokens = 0;
    m_promptTokens = 0;
    m_response.clear();
    m_trimmedResponse.clear();
    emit responseChanged(m_trimmedResponse);

    chatModel.updateSources        (responseIndex, {});
    chatModel.updateCurrentResponse(responseIndex, true);
    chatModel.updateStopped        (responseIndex, false);
    chatModel.updateThumbsUpState  (responseIndex, false);
    chatModel.updateThumbsDownState(responseIndex, false);
    chatModel.updateNewResponse    (responseIndex, "");
    currentChat.prompt(promptElement.promptPlusAttachments); // TODO: this isn't really what we want to do
#endif
}

ModelInfo ChatLLM::modelInfo() const
{
    return m_modelInfo;
}

void ChatLLM::setModelInfo(const ModelInfo &modelInfo)
{
    m_modelInfo = modelInfo;
    emit modelInfoChanged(modelInfo);
}

void ChatLLM::acquireModel()
{
    m_llModelInfo = LLModelStore::globalInstance()->acquireModel();
    emit loadedModelInfoChanged();
}

void ChatLLM::resetModel()
{
    m_llModelInfo = {};
    emit loadedModelInfoChanged();
}

void ChatLLM::modelChangeRequested(const ModelInfo &modelInfo)
{
    // ignore attempts to switch to the same model twice
    if (!isModelLoaded() || this->modelInfo() != modelInfo) {
        m_shouldBeLoaded = true;
        loadModel(modelInfo);
    }
}

static LLModel::PromptContext promptContextFromSettings(const ModelInfo &modelInfo)
{
    auto *mySettings = MySettings::globalInstance();
    return {
        .n_predict      = mySettings->modelMaxLength          (modelInfo),
        .top_k          = mySettings->modelTopK               (modelInfo),
        .top_p          = float(mySettings->modelTopP         (modelInfo)),
        .min_p          = float(mySettings->modelMinP         (modelInfo)),
        .temp           = float(mySettings->modelTemperature  (modelInfo)),
        .n_batch        = mySettings->modelPromptBatchSize    (modelInfo),
        .repeat_penalty = float(mySettings->modelRepeatPenalty(modelInfo)),
        .repeat_last_n  = mySettings->modelRepeatPenaltyTokens(modelInfo),
    };
}

void ChatLLM::prompt(QStringView prompt, const QStringList &enabledCollections)
{
    if (!isModelLoaded())
        return;

    try {
        promptInternal(prompt, enabledCollections, promptContextFromSettings(m_modelInfo));
    } catch (const std::exception &e) {
        // FIXME(jared): this is neither translated nor serialized
        emit responseChanged(e.what());
    }
}

auto ChatLLM::promptInternal(
    QStringView prompt, const QStringList &enabledCollections, const LLModel::PromptContext &ctx
) -> std::optional<PromptResult> {
    if (!isModelLoaded())
        return std::nullopt;

    Q_ASSERT(m_chatModel);

    auto *mySettings = MySettings::globalInstance();

    QList<ResultInfo> databaseResults;
    const int retrievalSize = mySettings->localDocsRetrievalSize();
    if (!enabledCollections.isEmpty()) {
        emit requestRetrieveFromDB(enabledCollections, prompt.toString(), retrievalSize, &databaseResults); // blocks
        emit databaseResultsChanged(databaseResults);
    }

    // Augment the prompt template with the results if any
    QString docsContext;
    if (!databaseResults.isEmpty()) {
        QStringList results;
        for (const ResultInfo &info : databaseResults)
            results << u"Collection: %1\nPath: %2\nExcerpt: %3"_s.arg(info.collection, info.path, info.text);

        // FIXME(jared): use a Jinja prompt template instead of hardcoded Alpaca-style localdocs template
        docsContext = u"### Context:\n%1\n\n"_s.arg(results.join("\n\n"));
    }

    // TODO(jared): pass this as a Jinja arg
#if 0
    if (!docsContext.isEmpty()) {
        auto old_n_predict = std::exchange(m_ctx.n_predict, 0); // decode localdocs context without a response
        m_llModelInfo.model->prompt(docsContext.toStdString(), "%1", promptFunc, responseFunc,
                                    /*allowContextShift*/ true, m_ctx);
        m_ctx.n_predict = old_n_predict; // now we are ready for a response
    }
#endif

    auto makeMap = [](const ChatItem &item) {
        return jinja2::GenericMap([msg = JinjaMessage(item)] { return &msg; });
    };

    std::string renderedPrompt;
    {
        auto items = m_chatModel->chatItems(); // holds lock

        jinja2::ValuesList messages;
        messages.reserve(items.size() + 1);
        for (auto &item : items)
            messages.emplace_back(makeMap(item));
        // TODO(jared): insert this earlier in *this* function... newPromptResponsePair is weird
        // messages.emplace_back(makeMap("user", prompt.toUtf8()));

        jinja2::ValuesMap params {
            { "messages",              std::move(messages) },
            { "bos_token",             "<|begin_of_text|>" },
            { "eos_token",             "<|eot_id|>"        },
            { "add_generation_prompt", true                },
        };

        auto promptTemplate = mySettings->modelPromptTemplate(m_modelInfo);
        jinja2::Template tmpl(jinjaEnv());
        auto maybeRendered = tmpl.Load(promptTemplate.toUtf8()).and_then([&tmpl, &params] {
            return tmpl.RenderAsString(params);
        });
        if (!maybeRendered)
            throw std::runtime_error(fmt::format("Failed to parse prompt template: {}", maybeRendered.error().ToString()));
        renderedPrompt = std::move(*maybeRendered);
    }

    PromptResult result;

    auto handlePrompt = [this, &result](std::span<const LLModel::Token> batch, bool cached) -> bool {
        Q_UNUSED(cached);
        result.promptTokens += batch.size();
        m_timer->start();
        return !m_stopGenerating;
    };

    auto handleResponse = [this, &result](LLModel::Token token, std::string_view piece) -> bool {
        result.responseTokens++;
        m_timer->inc();
        result.response.append(piece.data(), piece.size());
        auto respStr = QString::fromUtf8(result.response);
        emit responseChanged(removeLeadingWhitespace(respStr));
        return !m_stopGenerating;
    };

    QElapsedTimer totalTime;
    totalTime.start();
    m_timer->start();

    emit promptProcessing();
    m_llModelInfo.model->setThreadCount(mySettings->threadCount());
    m_stopGenerating = false;
    m_llModelInfo.model->prompt(renderedPrompt, handlePrompt, handleResponse, ctx);

    m_timer->stop();
    qint64 elapsed = totalTime.elapsed();

    SuggestionMode mode = mySettings->suggestionMode();
    if (mode == SuggestionMode::On || (!databaseResults.isEmpty() && mode == SuggestionMode::LocalDocsOnly))
        generateQuestions(elapsed);
    else
        emit responseStopped(elapsed);

    return result;
}

void ChatLLM::setShouldBeLoaded(bool b)
{
#if defined(DEBUG_MODEL_LOADING)
    qDebug() << "setShouldBeLoaded" << m_llmThread.objectName() << b << m_llModelInfo.model.get();
#endif
    m_shouldBeLoaded = b; // atomic
    emit shouldBeLoadedChanged();
}

void ChatLLM::requestTrySwitchContext()
{
    m_shouldBeLoaded = true; // atomic
    emit trySwitchContextRequested(modelInfo());
}

void ChatLLM::handleShouldBeLoadedChanged()
{
    if (m_shouldBeLoaded)
        reloadModel();
    else
        unloadModel();
}

void ChatLLM::unloadModel()
{
    if (!isModelLoaded() || m_isServer)
        return;

    if (!m_forceUnloadModel || !m_shouldBeLoaded)
        emit modelLoadingPercentageChanged(0.0f);
    else
        emit modelLoadingPercentageChanged(std::numeric_limits<float>::min()); // small non-zero positive value

#if defined(DEBUG_MODEL_LOADING)
    qDebug() << "unloadModel" << m_llmThread.objectName() << m_llModelInfo.model.get();
#endif

    if (m_forceUnloadModel) {
        m_llModelInfo.resetModel(this);
        m_forceUnloadModel = false;
    }

    LLModelStore::globalInstance()->releaseModel(std::move(m_llModelInfo));
}

void ChatLLM::reloadModel()
{
    if (isModelLoaded() && m_forceUnloadModel)
        unloadModel(); // we unload first if we are forcing an unload

    if (isModelLoaded() || m_isServer)
        return;

#if defined(DEBUG_MODEL_LOADING)
    qDebug() << "reloadModel" << m_llmThread.objectName() << m_llModelInfo.model.get();
#endif
    const ModelInfo m = modelInfo();
    if (m.name().isEmpty())
        loadDefaultModel();
    else
        loadModel(m);
}

void ChatLLM::generateName()
{
    Q_ASSERT(isModelLoaded());
    if (!isModelLoaded())
        return;

    auto *mySettings = MySettings::globalInstance();

    const QString chatNamePrompt = mySettings->modelChatNamePrompt(m_modelInfo);
    if (chatNamePrompt.trimmed().isEmpty()) {
        qWarning() << "ChatLLM: not generating chat name because prompt is empty";
        return;
    }

    QByteArray response; // raw UTF-8

    auto handleResponse = [this, &response](LLModel::Token token, std::string_view piece) -> bool {
        Q_UNUSED(token);

        response.append(piece.data(), piece.size());
        auto respStr = QString::fromUtf8(response);
        emit generatedNameChanged(respStr);
        QStringList words = respStr.simplified().split(' ', Qt::SkipEmptyParts);
        return words.size() <= 3;
    };

    // TODO: use Jinja template
    auto promptTemplate = mySettings->modelPromptTemplate(m_modelInfo);
    auto promptUtf8 = chatNamePrompt.toUtf8();
    m_llModelInfo.model->prompt(
        { promptUtf8.data(), size_t(promptUtf8.size()) },
        [this](auto &&...) { return !m_stopGenerating; },
        handleResponse,
        promptContextFromSettings(m_modelInfo),
        /*allowContextShift*/ false
    );
}

void ChatLLM::handleChatIdChanged(const QString &id)
{
    m_llmThread.setObjectName(id);
}

void ChatLLM::generateQuestions(qint64 elapsed)
{
    // FIXME: This only works with response by the model in english which is not ideal for a multi-language
    // model.
    // match whole question sentences
    static const std::regex reQuestion(R"(\b(?:What|Where|How|Why|When|Who|Which|Whose|Whom)\b[^?]*\?)");

    Q_ASSERT(isModelLoaded());
    if (!isModelLoaded()) {
        emit responseStopped(elapsed);
        return;
    }

    auto *mySettings = MySettings::globalInstance();

    QString suggestedFollowUpPrompt = mySettings->modelSuggestedFollowUpPrompt(m_modelInfo);
    if (suggestedFollowUpPrompt.trimmed().isEmpty()) {
        emit responseStopped(elapsed);
        return;
    }

    emit generatingQuestions();
    auto promptTemplate = mySettings->modelPromptTemplate(m_modelInfo);

    std::string response; // raw UTF-8

    auto handleResponse = [this, &response](LLModel::Token token, std::string_view piece) -> bool {
        Q_UNUSED(token);

        // add token to buffer
        response.append(piece);

        // extract all questions from response
        ptrdiff_t lastMatchEnd = -1;
        auto it = std::sregex_iterator(response.begin(), response.end(), reQuestion);
        auto end = std::sregex_iterator();
        for (; it != end; ++it) {
            auto pos = it->position();
            auto len = it->length();
            lastMatchEnd = pos + len;
            emit generatedQuestionFinished(QString::fromUtf8(&response[pos], len));
        }

        // remove processed input from buffer
        if (lastMatchEnd != -1)
            response.erase(0, lastMatchEnd);
        return true;
    };

    QElapsedTimer totalTime;
    totalTime.start();
    // TODO: use Jinja template
    auto promptUtf8 = suggestedFollowUpPrompt.toUtf8();
    m_llModelInfo.model->prompt(
        { promptUtf8.data(), size_t(promptUtf8.size()) },
        [this](auto &&...) { return !m_stopGenerating; },
        handleResponse,
        promptContextFromSettings(m_modelInfo),
        /*allowContextShift*/ false
    );
    elapsed += totalTime.elapsed();
    emit responseStopped(elapsed);
}

// this function serialized the cached model state to disk.
// we want to also serialize n_ctx, and read it at load time.
bool ChatLLM::serialize(QDataStream &stream, int version)
{
    if (version < 11) {
        if (version >= 2) {
            if (m_llModelType == LLModelTypeV1::NONE) {
                qWarning() << "ChatLLM ERROR: attempted to serialize a null model for chat id" << m_chat->id()
                           << "name" << m_chat->name();
                return false;
            }
            stream << m_llModelType;
            stream << 0; // state version
        }
        {
            QString dummy;
            stream << dummy; // response
            stream << dummy; // generated name
        }
        stream << quint32(0); // prompt + response tokens

        if (version < 6) { // serialize binary state
            if (version < 4) {
                stream << 0; // responseLogits
            }
            stream << int32_t(0); // n_past
            stream << quint64(0); // input token count
            stream << QByteArray(); // KV cache state
        }
    }
    return stream.status() == QDataStream::Ok;
}

bool ChatLLM::deserialize(QDataStream &stream, int version)
{
    // discard all state since we are initialized from the ChatModel as of v11
    if (version < 11) {
        union { int intval; quint32 u32; quint64 u64; };

        bool deserializeKV = true;
        if (version >= 6)
            stream >> deserializeKV;

        if (version >= 2) {
            stream >> intval; // model type
            auto llModelType = (version >= 6 ? parseLLModelTypeV1 : parseLLModelTypeV0)(intval);
            if (llModelType == LLModelTypeV1::NONE) {
                qWarning().nospace() << "error loading chat id " << m_chat->id() << ": unrecognized model type: "
                                     << intval;
                return false;
            }

            /* note: prior to chat version 10, API models and chats with models removed in v2.5.0 only wrote this because of
             * undefined behavior in Release builds */
            stream >> intval; // state version
            if (intval) {
                qWarning().nospace() << "error loading chat id " << m_chat->id() << ": unrecognized internal state version";
                return false;
            }
        }

        {
            QString dummy;
            stream >> dummy; // response
            stream >> dummy; // name response
        }
        stream >> u32; // prompt + response token count

        // We don't use the raw model state anymore.
        if (deserializeKV) {
            if (version < 4) {
                stream >> u32; // response logits
            }
            stream >> u32; // n_past
            if (version >= 7) {
                stream >> u32; // n_ctx
            }
            if (version < 9) {
                stream >> u64; // logits size
                stream.skipRawData(u64 * sizeof(float)); // logits
            }
            stream >> u64; // token cache size
            stream.skipRawData(u64 * sizeof(int)); // token cache
            QByteArray dummy;
            stream >> dummy; // state
        }
    }
    return stream.status() == QDataStream::Ok;
}
