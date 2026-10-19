#include "Exporter.h"

#include "Project.h"
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

void Exporter::start(const QUrl &file)
{
    if (m_running) return;
    QString path = file.toLocalFile();
    if (path.isEmpty()) return;
    if (!path.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) path += QStringLiteral(".mp4");

    const mtg::Draft draft = m_project->draft();
    m_cancel = false;
    m_running = true;
    m_progress = 0;
    emit runningChanged();
    emit progressChanged();

    m_thread = QThread::create([this, draft, path] {
        double reported = 0;
        const mtg::ExportResult res = mtg::exportDraft(draft, path, [&](double p) {
            if (p - reported >= 0.005 || p >= 1.0) {
                reported = p;
                QMetaObject::invokeMethod(this, [this, p] {
                    m_progress = p;
                    emit progressChanged();
                }, Qt::QueuedConnection);
            }
            return !m_cancel.load();
        });
        QMetaObject::invokeMethod(this, [this, res, path] {
            m_running = false;
            emit runningChanged();
            emit finished(res.ok, res.message, QUrl::fromLocalFile(path));
        }, Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}
