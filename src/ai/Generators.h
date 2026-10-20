#pragma once

// AI-generated media for ads, made by local models: images from ComfyUI
// (SDXL + Lightning), voiceovers from Kokoro and music from ACE-Step. Each
// backend runs asynchronously with progress and cancel. Files land in
// ~/Movies/Montage/Generated, named after what was asked for, so asking for
// the same thing again reuses the file.
//
// Edit plans ask for media with a "generate" list; the app makes the files,
// imports them and points the plan's references ("g1") at them before the
// plan is built:
//   "generate": [{"kind": "image", "prompt": "candles on a wooden table, warm light", "as": "g1"},
//                {"kind": "voiceover", "text": "Light up your evenings", "voice": "af_heart", "as": "g2"},
//                {"kind": "music", "prompt": "soft acoustic guitar", "seconds": 12, "as": "g3"}]

#include "model/Draft.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>

namespace mtg::ai {

struct GenRequest {
    QString kind;        // image | voiceover | music
    QString prompt;      // what the image or music should be
    QString text;        // the words a voiceover says
    QString voice;       // Kokoro voice id (af_heart, am_michael, bf_emma, ...)
    double seconds = 0;  // music length
    QString aspect;      // image shape: 16:9, 9:16, 1:1, 4:5 ...
    QString as;          // name later plan items use ("g1")

    // "Image · candles on a wooden table" for the chat.
    QString label() const;
    // Stable file name for this request: kind, a slug of its words and a hash
    // of everything that changes the result.
    QString fileName() const;
};

struct GenResult {
    bool ok = false;
    bool cancelled = false;
    QString path;
    QString error;
};

QStringList generateKinds();  // image, voiceover, music
QStringList voiceNames();     // Kokoro voices that ship with the model
// Kokoro voice for a description ("british male", "deep voice") or a voice id;
// empty when it names none.
QString voiceFor(const QString &description);

// Generated files are named "<kind>-<words>-<hash>.<ext>". Returns the kind
// ("image", "voiceover", "music") for such a file, else empty.
QString generatedKind(const QString &fileName);
// A voiceover or narration track: generated, or a file named like one.
bool isVoiceover(const Material &m);

// Where the backends live. Defaults match ~/ai-studio; every value can be
// overridden with the "generate/<key>" setting or MONTAGE_<KEY> in the
// environment (e.g. MONTAGE_COMFY_URL, MONTAGE_KOKORO_PYTHON).
struct GeneratorConfig {
    QString outputDir;     // ~/Movies/Montage/Generated
    QString comfyUrl;      // http://127.0.0.1:8188
    QString comfyDir;      // ~/ai-studio/comfyui, for the "how to start it" hint
    QString checkpoint;    // sd_xl_base_1.0.safetensors
    QString vae;           // sdxl_vae_fp16_fix.safetensors
    QString lora;          // sdxl_lightning_8step_lora.safetensors (8 steps, CFG 1)
    QString kokoroPython;  // a Python with kokoro installed
    QString kokoroModel;   // ~/ai-studio/models/kokoro-82m
    QString acePython;     // ~/ai-studio/ace-step/.venv/bin/python
    QString aceRoot;       // ~/ai-studio/ace-step
    QString aceModel;      // ~/ai-studio/models/ace-step-1.5
    QString ollamaUrl;     // its models are unloaded before heavy generation (24 GB Macs swap otherwise)
    bool freeMemory = true;

    static GeneratorConfig load();
};

// One backend. generate() works asynchronously and emits finished() exactly
// once, also after cancel().
class GeneratorBackend : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void generate(const GenRequest &request, const QString &outPath) = 0;
    virtual void cancel() = 0;
    // Empty when the backend can run; otherwise what's missing and how to fix
    // it. May block briefly (a server check).
    virtual QString unavailable() const { return {}; }

signals:
    void progress(double fraction, const QString &note);
    void finished(const GenResult &result);
};

// What a plan's "generate" list turned into.
struct GenResolution {
    QJsonObject plan;              // references bound, "generate" removed
    QHash<QString, QString> made;  // "g1" -> media alias ("m5")
    QStringList generated;         // media aliases of the generated files, in request order
    QStringList errors;            // items that failed; plan items using them are left out
    QStringList failedKinds;       // kinds of the items that failed
    bool cancelled = false;
};

// Runs generate requests one after another (the models share the GPU) and
// resolves plans that ask for media.
class Generators : public QObject
{
    Q_OBJECT
public:
    explicit Generators(QObject *parent = nullptr);  // real backends, configured by GeneratorConfig::load()
    Generators(const GeneratorConfig &config, QObject *parent);

    // Replaces the backend for a kind (tests use fakes). Takes ownership.
    void setBackend(const QString &kind, GeneratorBackend *backend);
    const GeneratorConfig &config() const { return m_config; }
    bool running() const { return m_index >= 0; }

    // Empty when `kind` can run, else what's missing and how to fix it.
    QString unavailable(const QString &kind) const;

    // Makes the files for `requests`, skipping ones that already exist.
    void run(const QList<GenRequest> &requests);
    void cancel();

    // Imports files into the project and returns the draft afterwards.
    using ImportFn = std::function<const Draft &(const QStringList &paths)>;
    // Runs the plan's "generate" items, imports the files and binds the plan's
    // references to them. Images default to the plan's shape (else the
    // canvas's). `done` is called once.
    void resolve(const QJsonObject &plan, const Draft &current, const ImportFn &import,
                 const std::function<void(const GenResolution &)> &done);

signals:
    void itemStarted(int index, int count, const QString &label);
    void itemProgress(int index, double fraction, const QString &note);
    void itemFinished(int index, const GenResult &result);
    void finished(const QList<GenResult> &results, bool cancelled);

private:
    void next();
    void freeMemoryFor(const QString &kind);

    GeneratorConfig m_config;
    QHash<QString, GeneratorBackend *> m_backends;
    QList<GenRequest> m_queue;
    QList<GenResult> m_results;
    int m_index = -1;
    bool m_cancelled = false;
    QString m_lastHeavy;  // kind that last used the GPU
};

// The plan's "generate" items as requests. Items without a name get "g1",
// "g2"... in order; items missing what they need are dropped with a warning.
QList<GenRequest> generateRequests(const QJsonObject &plan, QStringList *warnings = nullptr);

// Points "media" references to generated names ("g1") at the media aliases in
// `made` and removes "generate". Items using a name that wasn't made (it
// failed) are left out. Generated audio is clamped to its file silently (the
// model couldn't know its length).
void bindGenerated(const QHash<QString, QString> &made, const Draft &d, QJsonObject *plan);

}  // namespace mtg::ai
