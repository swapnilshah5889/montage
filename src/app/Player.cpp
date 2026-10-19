#include "Player.h"

#include "Project.h"
#include "engine/PreviewEngine.h"

Player::Player(Project *project, QObject *parent)
    : QObject(parent)
    , m_project(project)
    , m_engine(new mtg::PreviewEngine)
{
    s_instance = this;
    m_thread.setObjectName(QStringLiteral("preview"));
    m_engine->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_engine, &QObject::deleteLater);

    connect(m_engine, &mtg::PreviewEngine::frameReady, this, [this](const QImage &img, qint64 t) {
        m_frame = img;
        emit frameReady(img);
        if (m_playing) setPosition(t);
    });
    connect(m_engine, &mtg::PreviewEngine::stopped, this, [this](qint64 t) {
        if (m_playing) {
            m_playing = false;
            emit playingChanged();
        }
        setPosition(t);
    });

    connect(project, &Project::changed, this, &Player::pushDraft);
    connect(project, &Project::liveChanged, this, &Player::pushDraft);
    m_thread.start();
    pushDraft();
}

Player::~Player()
{
    m_thread.quit();
    m_thread.wait();
}

Player *Player::create(QQmlEngine *, QJSEngine *)
{
    Q_ASSERT(s_instance);
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void Player::pushDraft()
{
    const mtg::Draft draft = m_project->draft();
    QMetaObject::invokeMethod(m_engine, [e = m_engine, draft] { e->setDraft(draft); }, Qt::QueuedConnection);
    if (m_position > draft.duration()) seek(draft.duration());
}

void Player::setPosition(qint64 t)
{
    if (t == m_position) return;
    m_position = t;
    emit positionChanged();
}

void Player::play()
{
    if (m_playing) return;
    m_playing = true;
    emit playingChanged();
    const qint64 from = m_position;
    QMetaObject::invokeMethod(m_engine, [e = m_engine, from] { e->play(from); }, Qt::QueuedConnection);
}

void Player::pause()
{
    if (!m_playing) return;
    m_playing = false;
    emit playingChanged();
    QMetaObject::invokeMethod(m_engine, &mtg::PreviewEngine::pause, Qt::QueuedConnection);
}

void Player::toggle()
{
    m_playing ? pause() : play();
}

void Player::seek(qint64 t)
{
    t = std::clamp<qint64>(t, 0, std::max<qint64>(0, m_project->duration()));
    setPosition(t);
    m_engine->requestSeek(t);
}

void Player::step(int frames)
{
    pause();
    const double fps = m_project->fps() > 0 ? m_project->fps() : 30.0;
    seek(m_position + qint64(frames * 1'000'000.0 / fps));
}

void Player::setPreviewBox(int width, int height)
{
    const QSize box(width, height);
    QMetaObject::invokeMethod(m_engine, [e = m_engine, box] { e->setPreviewBox(box); }, Qt::QueuedConnection);
}
