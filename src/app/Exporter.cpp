#include "Exporter.h"

#include "Project.h"
#include "ai/EditPlan.h"
#include "engine/ExportJob.h"

Exporter::Exporter(Project *project, QObject *parent)
    : QObject(parent)
    , m_project(project)
{
    s_instance = this;
}

Exporter::~Exporter()
{
    m_cancel = true;
    if (m_thread) m_thread->wait();
}

Exporter *Exporter::create(QQmlEngine *, QJSEngine *)
{
    Q_ASSERT(s_instance);
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

QStringList Exporter::formatNames() const
{
    return mtg::ai::exportFormatNames();
}

QStringList Exporter::outputPaths(const QString &file, const QStringList &formats)
{
    QString path = file;
    if (!path.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) path += QStringLiteral(".mp4");
    if (formats.isEmpty()) return {path};
    const QString base = path.left(path.size() - 4);
    QStringList out;
    for (const QString &f : formats) out << QStringLiteral("%1 %2.mp4").arg(base, QString(f).replace(QLatin1Char(':'), QLatin1Char('x')));
    return out;
}

void Exporter::start(const QUrl &file, const QStringList &formats)
{
    if (m_running) return;
    const QString local = file.toLocalFile();
    if (local.isEmpty()) return;

    // One draft per output, laid out for its shape up front (on this thread).
    struct Job {
        mtg::Draft draft;
        QString path, label;
    };
    QList<Job> jobs;
    const QStringList paths = outputPaths(local, formats);
    if (formats.isEmpty()) {
        jobs.append({m_project->draft(), paths[0], {}});
    } else {
        for (qsizetype i = 0; i < formats.size(); ++i)
            jobs.append({mtg::ai::resizedDraft(m_project->draft(), formats[i]), paths[i], formats[i]});
    }

    m_cancel = false;
    m_running = true;
    m_progress = 0;
    m_stage = jobs.size() > 1 ? QStringLiteral("%1 · 1 of %2").arg(jobs[0].label).arg(jobs.size()) : QString();
    emit runningChanged();
    emit progressChanged();

    m_thread = QThread::create([this, jobs] {
        mtg::ExportResult res;
        QStringList written;
        const int n = int(jobs.size());
        for (int k = 0; k < n; ++k) {
            const Job &job = jobs[k];
            const QString stage = n > 1 ? QStringLiteral("%1 · %2 of %3").arg(job.label).arg(k + 1).arg(n) : QString();
            double reported = -1;
            res = mtg::exportDraft(job.draft, job.path, [&](double p) {
                const double total = (k + p) / n;
                if (total - reported >= 0.005 || p >= 1.0) {
                    reported = total;
                    QMetaObject::invokeMethod(this, [this, total, stage] {
                        m_progress = total;
                        m_stage = stage;
                        emit progressChanged();
                    }, Qt::QueuedConnection);
                }
                return !m_cancel.load();
            });
            if (!res.ok) break;
            written << job.path;
        }
        QString message = res.message;
        if (res.ok && written.size() > 1) message = QStringLiteral("Exported %1 formats").arg(written.size());
        QMetaObject::invokeMethod(this, [this, res, written, message, first = jobs.first().path] {
            m_running = false;
            m_stage.clear();
            emit runningChanged();
            emit finished(res.ok, message, QUrl::fromLocalFile(written.value(0, first)), written);
        }, Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}
