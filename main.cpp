#include <QCoreApplication>
#include <QSettings>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QTextStream>
#include <QDebug>
#include <QHash>
#include <QSet>

#include <windows.h>

#define QUERY_ERROR -1

struct DbConfig
{
    QString host;
    int     port = 5432;
    QString name;
    QString user;
    QString password;
};

static void setupUtf8(QTextStream &stream)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    stream.setCodec("UTF-8");
#else
    Q_UNUSED(stream);
#endif
}

// Подключение к БД
bool initDb(QSqlDatabase &db, const DbConfig &cfg);

// Получение id документа по принципу upsert
int getDocumentId(QSqlDatabase &db, const QString &path);

// Получение id слова по принципу upsert
int getWordId(QSqlDatabase &db, const QString &word);

// Индексация одного файла
bool indexFile(QSqlDatabase &db, const QString &filePath);

// Рекурсивный обход каталога
void scanDirectory(QSqlDatabase &db,const QString &root,const QSet<QString> &extensions);

QSet<QString> readExtensions(QSettings &settings);
QStringList readDirectories(QSettings &settings);
DbConfig readDbConfig(QSettings &settings);


int main(int argc, char *argv[])
{
    SetConsoleOutputCP(CP_UTF8);

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QString("Indexer"));

    QSettings settings(QString("indexer.ini"), QSettings::IniFormat);

    QStringList directories;


    if (directories.isEmpty())
    {
       qDebug() << "Неверно задан каталог для индексации";
       return EXIT_FAILURE;
    }

    const QSet<QString> extensions = readExtensions(settings);
    const DbConfig cfg = readDbConfig(settings);

    QSqlDatabase db;
    if (!initDb(db, cfg))
        return EXIT_FAILURE;

    for (const QString &dirPath : directories) {
        QDir dir(dirPath);

        if (!dir.exists()) {
            qDebug() << "Каталог не существует";
            continue;
        }

        scanDirectory(db, dir.absolutePath(), extensions);
    }

    db.close();
    qDebug() << "Индексация завершена.";
    return EXIT_SUCCESS;
}

bool initDb(QSqlDatabase &db, const DbConfig &cfg)
{
    db = QSqlDatabase::addDatabase(QString("QPSQL"));

    db.setHostName(cfg.host);
    db.setPort(cfg.port);
    db.setDatabaseName(cfg.name);
    db.setUserName(cfg.user);
    db.setPassword(cfg.password);

    if (!db.open()) {
        qDebug() << "Ошибка подключения к базе данных";
        return false;
    }

    qDebug() << "Подключение установлено";

    QSqlQuery query(db);

    std::vector<QString> sqlMessage;


    sqlMessage.push_back( "CREATE TABLE IF NOT EXISTS documents ("
                          " id   SERIAL PRIMARY KEY,"
                          " path TEXT NOT NULL UNIQUE"
                          ")"
                             );

    sqlMessage.push_back(  "CREATE TABLE IF NOT EXISTS documents ("
                           " id   SERIAL PRIMARY KEY,"
                           " path TEXT NOT NULL UNIQUE"
                           ")"
                        );

    sqlMessage.push_back(  "CREATE TABLE IF NOT EXISTS words ("
                           " id   SERIAL PRIMARY KEY,"
                           " word TEXT NOT NULL UNIQUE"
                           ")"
                        );

    sqlMessage.push_back(   "CREATE TABLE IF NOT EXISTS word_frequencies ("
                            " document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,"
                            " word_id     INTEGER NOT NULL REFERENCES words(id)     ON DELETE CASCADE,"
                            " frequency   INTEGER NOT NULL,"
                            " PRIMARY KEY (document_id, word_id)"
                            ")"
                        );

    sqlMessage.push_back(   "CREATE INDEX IF NOT EXISTS idx_word_frequencies_word_id "
                            "ON word_frequencies(word_id)"
                        );


    for (const QString &sql : sqlMessage)
    {
        if (!query.exec(sql)) {
            qDebug() << "Ошибка создания схемы БД";
            return false;
        }
    }

    return true;
}


int getDocumentId(QSqlDatabase &db, const QString &path)
{
    QSqlQuery query(db);

    QString strMessageTemp = "INSERT INTO documents(path) VALUES(" + path + ") \
                              ON CONFLICT (path) DO NOTHING";
    if (!query.exec(strMessageTemp)) {
        return QUERY_ERROR;
    }


    strMessageTemp = "SELECT id FROM documents WHERE path =" + path;
    if (!query.exec(strMessageTemp) || !query.next()) {
        return QUERY_ERROR;
    }

    return query.value(0).toInt();
}

int getWordId(QSqlDatabase &db, const QString &word)
{
    QSqlQuery query(db);

    QString strMessageTemp =  "INSERT INTO words(word) VALUES(" + word +") \
                               ON CONFLICT (word) DO NOTHING";
    if (!query.exec(strMessageTemp)) {
        return QUERY_ERROR;
    }


    strMessageTemp = "SELECT id FROM words WHERE word = " + word;
    if (!query.exec(strMessageTemp) || !query.next()) {
        return QUERY_ERROR;
    }

    return query.value(0).toInt();
}

bool indexFile(QSqlDatabase &db, const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qDebug() << "Не удалось открыть файл";
        return false;
    }

    QTextStream stream(&file);
    setupUtf8(stream);

    const QString text = stream.readAll();
    file.close();

    QHash<QString, int> freq;
    QString current;
    QString sqlMessageTemp;

    for (QChar ch : text)
    {
        if (ch.isLetterOrNumber())
        {
            current.append(ch.toLower());
            continue;
        }

        // Разделение и условие из задания
        if (!current.isEmpty())
        {
            if (current.size() >= 3 && current.size() <= 32)
                ++freq[current];
            current.clear();
        }
    }
    if (!current.isEmpty())
    {
        if (current.size() >= 3 && current.size() <= 32)
            ++freq[current];
        current.clear();
    }

    if (freq.isEmpty())
        return true;


    if (!db.transaction())
        return false;



    const int docId = getDocumentId(db, filePath);
    if (docId == QUERY_ERROR)
    {
        db.rollback();
        qDebug() << "Ошибка получения документ";
        return false;
    }

    QSqlQuery del(db);

    sqlMessageTemp = "DELETE FROM word_frequencies WHERE document_id = " + QString::number(docId);

    if (!del.exec(sqlMessageTemp))
    {
        db.rollback();
        qDebug() << "Ошибка очистки старых частот";
        return false;
    }

    QSqlQuery insertFreq(db);

    for (auto it = freq.cbegin(); it != freq.cend(); ++it) {
        const int wordId = getWordId(db, it.key());
        if (wordId == QUERY_ERROR)
        {
            db.rollback();
            qDebug() << "Ошибка получения слова";
            return false;
        }

        sqlMessageTemp =  "INSERT INTO word_frequencies(document_id, word_id, frequency) \
                          VALUES(" + QString::number(docId) + ", " + QString::number(wordId) + ", " +  QString::number(it.value()) + ") \
                          ON CONFLICT (document_id, word_id )\
                          DO UPDATE SET frequency = EXCLUDED.frequency";

        if (!insertFreq.exec(sqlMessageTemp))
        {
            db.rollback();
            return false;
        }
    }

    if (!db.commit())
    {
        qDebug() << "Ошибка фиксации транзакции";
        return false;
    }

    return true;
}

void scanDirectory(QSqlDatabase &db,const QString &root,const QSet<QString> &extensions)
{
    QDirIterator it(root,
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
        const QString filePath = it.next();
        const QFileInfo info(filePath);
        const QString ext = info.suffix().toLower();

        if (!extensions.contains(ext))
            continue;

        indexFile(db, filePath);
    }
}

QSet<QString> readExtensions(QSettings &settings)
{
    const QString raw = settings.value(
        QString("Indexer/Extensions"),
        QString("txt,md,ini,csv")
    ).toString();

    QSet<QString> result;
    for (QString ext : raw.split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        ext = ext.trimmed().toLower();

        if (ext.startsWith(QLatin1Char('.')))
            ext.remove(0, 1);

        if (!ext.isEmpty())
            result.insert(ext);
    }

    return result;
}

QStringList readDirectories(QSettings &settings)
{
    const QString raw = settings.value(
        QString("Indexer/Directories")
    ).toString();

    QStringList result;
    for (QString dir : raw.split(QLatin1Char(';'), Qt::SkipEmptyParts))
    {
        dir = dir.trimmed();
        if (!dir.isEmpty())
            result << dir;
    }

    return result;
}

DbConfig readDbConfig(QSettings &settings)
{
    DbConfig cfg;
    cfg.host     = settings.value(QString("Database/Host"),     QString("localhost")).toString();
    cfg.port     = settings.value(QString("Database/Port"),     5432).toInt();
    cfg.name     = settings.value(QString("Database/Name"),     QString("indexer")).toString();
    cfg.user     = settings.value(QString("Database/User"),     QString("postgres")).toString();
    cfg.password = settings.value(QString("Database/Password"), QString()).toString();
    return cfg;
}
