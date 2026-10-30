#include <QtTest>
#include <QtWidgets>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QAudioDecoder>
#include <QTemporaryDir>
#include <QProcess>
#include <QtEndian>
#include "ttsclient.h"
#include "sttservice.h"
#include "audiomerger.h"
#include "numberededitor.h"
#include "settingsdialog.h"
// Focused integration access, without production-only test hooks.
#define private public
#include "mainwindow.h"
#undef private

class UiFlowTest : public QObject {
    Q_OBJECT
    QProcess fixture;
    QTemporaryDir scratch;
    QString base;
    static QString controlGeometry(QWidget *control,QWidget *container) {
        QString details;QDebug stream(&details);
        stream.nospace()<<control->objectName()<<" bounds="<<QRect(control->mapTo(container,QPoint()),control->size())
            <<" container="<<container->rect()<<" hint="<<control->sizeHint()<<" minimum="<<control->minimumSizeHint()
            <<" containerMinimum="<<container->minimumSizeHint()<<" containerHfw="<<container->heightForWidth(container->width())
            <<" window="<<control->window()->size()<<" font="<<QFontInfo(control->font()).family()
            <<" pixels="<<QFontInfo(control->font()).pixelSize()<<" scale="<<control->devicePixelRatioF();
        return details;
    }
    void prepare(MainWindow &window, const QString &path = "/json") {
        auto profile = defaultServerProfile();
        profile["baseUrl"] = base;
        profile["adapter"] = "openai";
        profile["speechPath"] = path;
        profile["voice"] = "fixture";
        profile["responseMode"] = "auto";
        profile["timeoutSeconds"] = 4;
        window.m_settings["profiles"] = QJsonArray{profile};
        window.m_settings["selectedProfile"] = 0;
        window.m_settings["highlight"] = true;
        window.m_settings["follow"] = true;
        window.updateProfileChoices();
        window.m_demo = true;
        window.m_text->setPlainText("Hello world. Hello world.");
        window.resize(900, 680);
        window.show();
        QCoreApplication::processEvents();
    }
    static void dispatchEscape(MainWindow &window) {
        for (auto *shortcut : window.findChildren<QShortcut *>()) {
            if (shortcut->key() == QKeySequence(Qt::Key_Escape)) {
                QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection);
                return;
            }
        }
        QFAIL("Escape shortcut was not found");
    }
private slots:
    void initTestCase() {
        // Exercise the same base style and font as the shipped application.
        QApplication::setStyle("Fusion");
        QFont font=qApp->font();
#ifdef Q_OS_WIN
        QVERIFY2(!QFontDatabase::families().isEmpty(),"UI geometry tests require real fonts. Set QT_QPA_FONTDIR to the Windows Fonts directory for the offscreen platform.");
        font.setFamily("Segoe UI");
#else
        font.setFamily("DejaVu Sans");
#endif
        if(!QFontDatabase::families().contains(font.family()))font.setFamily("DejaVu Sans");
        font.setPointSize(10);qApp->setFont(font);
        QVERIFY(scratch.isValid());
        qputenv("SPEECHDESK_CONFIG_DIR", scratch.path().toUtf8());
        QFile settings(scratch.filePath("settings.json"));
        QVERIFY(settings.open(QIODevice::WriteOnly));
        settings.write("{\"theme\":\"dark\"}"); settings.close();
        auto path = qEnvironmentVariable("SPEECHDESK_FIXTURE");
        if (path.isEmpty()) path = QDir::currentPath() + "/tests/tts_fixture_server.py";
        fixture.start(qEnvironmentVariable("SPEECHDESK_PYTHON", "python3"), {path});
        QVERIFY(fixture.waitForStarted(5000));
        QVERIFY(fixture.waitForReadyRead(5000));
        const auto port = QString::fromUtf8(fixture.readLine()).trimmed();
        QVERIFY(port.toInt() > 0);
        base = "http://127.0.0.1:" + port;
    }
    void cleanupTestCase() { fixture.terminate(); fixture.waitForFinished(3000); }
    void playbackHighlightPauseReplayAndExport() {
        MainWindow window; prepare(window);
        QSignalSpy audio(&window.m_tts, &TtsClient::audioReady);
        QSignalSpy failures(&window.m_tts, &TtsClient::error);
        QSignalSpy playerFailures(&window.m_player, &QMediaPlayer::errorOccurred);
        window.startSpeech();
        QTRY_VERIFY_WITH_TIMEOUT(window.m_player.playbackState() == QMediaPlayer::PlayingState, 5000);
        QVERIFY(window.m_active); QCOMPARE(window.m_segments.size(), 2);QVERIFY(window.m_status->text().startsWith("Reading part "));
        QVERIFY(!window.m_cues[0].isEmpty());
        // The UI must use actual server times, not a word-rate estimate.
        window.updateHighlight(50);
        const auto selections = window.m_text->extraSelections();
        QCOMPARE(selections.size(), 2);
        QCOMPARE(selections[1].cursor.selectedText(), QString("Hello"));
        window.pauseSpeech(); QVERIFY(window.m_paused);
        QTRY_COMPARE(window.m_player.playbackState(), QMediaPlayer::PausedState);
        const auto paused = window.m_player.position();
        QTest::qWait(220);
        QVERIFY(qAbs(window.m_player.position() - paused) <= 25);
        window.pauseSpeech(); QVERIFY(!window.m_paused);
        QTRY_VERIFY_WITH_TIMEOUT(!window.m_active, 5000);
        QVERIFY2(window.m_cacheComplete, qPrintable(window.m_status->text()));QVERIFY(!window.m_status->text().contains("Reading part"));
        QCOMPARE(audio.size(), 2); QCOMPARE(failures.size(), 0); QCOMPARE(playerFailures.size(), 0);
        QVERIFY(window.m_text->extraSelections().isEmpty());
        QVERIFY(window.m_export->isEnabled());

        const auto cachedPaths = window.m_audioPaths;
        audio.clear();
        window.startSpeech();
        QTRY_VERIFY_WITH_TIMEOUT(window.m_player.playbackState() == QMediaPlayer::PlayingState, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.m_active, 5000);
        QCOMPARE(audio.size(), 0); QCOMPARE(window.m_audioPaths, cachedPaths);

        const auto target = scratch.filePath("complete.wav");
        QSignalSpy exported(&window.m_merger, &AudioMerger::completed);
        QSignalSpy exportErrors(&window.m_merger, &AudioMerger::error);
        QStringList files; for (const auto &file : cachedPaths) files << file;
        window.m_speak->setEnabled(false);
        window.m_merger.start(files, target);
        QTRY_VERIFY_WITH_TIMEOUT(exported.size() + exportErrors.size() > 0, 8000);
        QVERIFY2(exportErrors.isEmpty(), exportErrors.isEmpty() ? "" : qPrintable(exportErrors.first()[0].toString()));
        QCOMPARE(exported.size(), 1);
        QFile output(target); QVERIFY(output.open(QIODevice::ReadOnly)); const auto bytes = output.readAll();
        QVERIFY(bytes.startsWith("RIFF")); QVERIFY(bytes.mid(8, 4) == "WAVE");
        const auto rate = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + 24));
        const auto dataBytes = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + 40));
        QCOMPARE(rate, quint32(48000)); QCOMPARE(dataBytes, quint32(bytes.size() - 44));
        const double duration = double(dataBytes) / (48000.0 * 2.0);
        QVERIFY2(qAbs(duration - 0.8) < 0.01, qPrintable(QString::number(duration)));
        QVERIFY(window.m_speak->isEnabled()); QVERIFY(window.m_export->isEnabled());

        QStringList many; for (int i = 0; i < 1000; ++i) many << files.first();
        const auto cancelledTarget = scratch.filePath("cancelled.wav");
        window.m_speak->setEnabled(false);
        window.m_merger.start(many, cancelledTarget); QVERIFY(window.m_merger.busy());
        dispatchEscape(window);
        QVERIFY(!window.m_merger.busy()); QVERIFY(window.m_speak->isEnabled());
        QVERIFY(window.m_export->isEnabled()); QVERIFY(!QFile::exists(cancelledTarget));
    }
    void stopSuppressesStaleNetworkAndSectionTransition() {
        MainWindow window; prepare(window, "/delay");
        QSignalSpy audio(&window.m_tts, &TtsClient::audioReady);
        window.startSpeech(); QVERIFY(window.m_active); window.stopSpeech();
        prepare(window, "/json");
        window.startSpeech();
        // Exercise a queued end-of-section transition from this session, then
        // stop and restart before that timer can run.
        window.m_player.mediaStatusChanged(QMediaPlayer::EndOfMedia);
        window.stopSpeech(); window.startSpeech();
        QCOMPARE(window.m_current, 0);
        QCoreApplication::processEvents(); QCOMPARE(window.m_current, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!window.m_active, 5000);
        QVERIFY2(window.m_cacheComplete, qPrintable(window.m_status->text()));
        QCOMPARE(audio.size(), 2);
        QTest::qWait(1700);
        QCOMPARE(audio.size(), 2); QVERIFY(!window.m_active);
    }
    void highlightAutoscrollPreservesCaret() {
        NumberedEditor editor; editor.resize(600, 300);
        QString text; for (int i = 0; i < 120; ++i) text += QString("Line %1 with text\n").arg(i);
        editor.setPlainText(text); editor.show(); QCoreApplication::processEvents();
        editor.moveCursor(QTextCursor::Start);
        const int target = text.indexOf("Line 100 ");
        editor.setReadingRange(target, 10, -1, 0, true);
        QCoreApplication::processEvents();
        QTextCursor cursor(editor.document()); cursor.setPosition(target);
        QVERIFY(editor.viewport()->rect().contains(editor.cursorRect(cursor)));
        QCOMPARE(editor.textCursor().position(), 0);
        QVERIFY(editor.verticalScrollBar()->value() > 0);
    }
    void cleanStartupAndThemesPreserveTextModificationState() {
        MainWindow window;
        QVERIFY(window.m_text->toPlainText().isEmpty());
        QVERIFY(!window.m_text->document()->isModified());
        window.show(); QCoreApplication::processEvents();
        QVERIFY(!window.m_text->document()->isModified());
        window.setThemeOverride("light"); QCoreApplication::processEvents();
        QVERIFY(!window.m_text->document()->isModified());
        window.m_text->setPlainText("Loaded text");
        window.m_text->document()->setModified(false);
        window.setThemeOverride("dark"); QCoreApplication::processEvents();
        QCOMPARE(window.m_text->toPlainText(), QString("Loaded text"));
        QVERIFY(!window.m_text->document()->isModified());
        window.m_text->moveCursor(QTextCursor::End);
        window.m_text->insertPlainText(" edited");
        QVERIFY(window.m_text->document()->isModified());
        window.setThemeOverride("light"); QCoreApplication::processEvents();
        QCOMPARE(window.m_text->toPlainText(), QString("Loaded text edited"));
        QVERIFY(window.m_text->document()->isModified());
        window.m_settings["fontSize"] = 20;
        window.m_lastTheme.clear(); window.applyTheme(); QCoreApplication::processEvents();
        QVERIFY(window.m_text->document()->isModified());
        window.m_text->document()->setModified(false);
        window.setThemeOverride("dark"); QCoreApplication::processEvents();
        QVERIFY(!window.m_text->document()->isModified());
    }
    void unifiedEditorFitsMinimumSizeWithDictation() {
        MainWindow window; prepare(window);
        window.showDemo(true);
        window.resize(680, 540);
        QCoreApplication::processEvents();
        QCOMPARE(window.size(), QSize(680, 540));
        QVERIFY(window.m_dictationPanel->isVisible());
        QVERIFY(window.m_text->height() >= 54);
        const auto area = window.centralWidget()->rect();
        for (QWidget *control : QList<QWidget *>{window.m_text,
                                window.m_record, window.m_cancelStt,
                                window.m_speak, window.m_export}) {
            const QRect bounds(control->mapTo(window.centralWidget(), QPoint()), control->size());
            QVERIFY2(area.contains(bounds), qPrintable(control->objectName()));
        }
    }
    void microphoneMeterIsCenteredAndLiveButtonKeepsState() {
        MainWindow window;window.showDemo(true);window.resize(680,540);window.show();
        QCoreApplication::processEvents();
        QVERIFY(window.m_level->isVisible());QVERIFY(window.m_live->isCheckable());
        QCOMPARE(window.m_live->height(),window.m_record->height());
        QCOMPARE(window.m_live->height(),window.m_transcribeFile->height());
        auto *bar=window.m_status->parentWidget();
        const auto centered=[&]{return qAbs(window.m_level->mapTo(bar,window.m_level->rect().center()).x()-bar->rect().center().x())<=1;};
        QVERIFY(centered());
        window.m_status->setText("A much longer dictation status message to exercise asymmetric label widths.");
        window.m_recordTime->setText("12:34");QCoreApplication::processEvents();QVERIFY(centered());
        const bool before=window.m_live->isChecked();window.m_live->click();QCOMPARE(window.m_live->isChecked(),!before);
        window.m_live->click();QCOMPARE(window.m_live->isChecked(),before);
        window.m_level->setValue(85);QCOMPARE(window.m_level->value(),85);
        window.updateSttControls();QCOMPARE(window.m_level->value(),0);
        window.m_dictate->setChecked(false);QVERIFY(!window.m_level->isVisible());
    }
    void fullControlTextAndCenteredDictationActions() {
        MainWindow window;window.showDemo(true);window.show();
        for(const QSize target:{QSize(680,540),QSize(1100,760)}) {
            window.resize(target);QCoreApplication::processEvents();
            QCOMPARE(window.size(),target);
            auto *actions=window.findChild<QWidget *>("dictationActions");QVERIFY(actions);
            auto *selectors=window.findChild<QWidget *>("dictationSelectors");QVERIFY(selectors);
            QCOMPARE(window.m_sttModel->mapTo(selectors,QPoint()).x(),selectors->contentsRect().left());
            const QRect refreshBounds(window.m_refreshDevices->mapTo(selectors,QPoint()),window.m_refreshDevices->size());
            QCOMPARE(refreshBounds.right(),selectors->contentsRect().right());
            for(QComboBox *box:{window.m_sttModel,window.m_language,window.m_device}) {
                QCOMPARE(box->sizePolicy().horizontalPolicy(),QSizePolicy::Expanding);
                QCOMPARE(box->mapTo(selectors,QPoint()).y(),window.m_sttModel->mapTo(selectors,QPoint()).y());
            }
            QRect buttonBounds;
            for(QPushButton *button:{window.m_record,window.m_cancelStt,window.m_transcribeFile,static_cast<QPushButton *>(window.m_live)}) {
                button->ensurePolished();
                QFont measuredFont=button->font();if(button==window.m_live)measuredFont.setWeight(QFont::DemiBold);
                const QFontMetricsF metrics(measuredFont,button);
                QVERIFY2(button->width()>=qCeil(metrics.horizontalAdvance(button->text()))+26,qPrintable(button->text()));
                QVERIFY2(button->height()>=qCeil(metrics.height())+16,qPrintable(button->text()));
                QVERIFY(button->size().expandedTo(button->sizeHint())==button->size());
                const QRect bounds(button->mapTo(actions,QPoint()),button->size());
                QVERIFY2(actions->rect().contains(bounds),qPrintable(controlGeometry(button,actions)));buttonBounds=buttonBounds.united(bounds);
            }
            QVERIFY2(qAbs(buttonBounds.center().x()-actions->rect().center().x())<=1,qPrintable(controlGeometry(window.m_live,actions)));
            QCOMPARE(window.m_live->text(),QString("Live text"));
            for(QComboBox *box:{window.m_server,window.m_model,window.m_voice,window.m_sttModel,window.m_language,window.m_device}) {
                QStyleOptionComboBox option;option.initFrom(box);option.editable=box->isEditable();option.frame=true;
                const QRect textRect=box->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,box);
                const QString visible=box->currentText().isEmpty()&&box->lineEdit()?box->lineEdit()->placeholderText():box->currentText();
                const QFontMetrics metrics(box->font(),box);
                QVERIFY2(textRect.width()>=metrics.horizontalAdvance(visible)+4||box->height()>box->sizeHint().height(),qPrintable(visible));
                QCOMPARE(box->view()->textElideMode(),Qt::ElideNone);
                auto *list=qobject_cast<QListView *>(box->view());QVERIFY(list);QVERIFY(list->wordWrap());
                const QRect bounds(box->mapTo(window.centralWidget(),QPoint()),box->size());
                QVERIFY2(window.centralWidget()->rect().contains(bounds),qPrintable(box->objectName()));
            }
            window.m_live->setChecked(!window.m_live->isChecked());QCoreApplication::processEvents();
            QVERIFY(window.m_live->width()>=window.m_live->minimumSizeHint().width());
        }
        // Device names can exceed a compact row without losing any characters.
        window.resize(680,540);
        window.m_device->addItem(QString("USB microphone with an unusually long manufacturer and product name for the recording input ").repeated(3));
        window.m_device->setCurrentIndex(window.m_device->count()-1);QCoreApplication::processEvents();
        QCOMPARE(window.size(),QSize(680,540));
        QVERIFY(window.m_text->height()>=54);
        QVERIFY(window.m_device->hasHeightForWidth());
        QTRY_VERIFY2_WITH_TIMEOUT(window.m_device->height()>=window.m_device->heightForWidth(window.m_device->width()),qPrintable(controlGeometry(window.m_device,window.m_device->parentWidget())),500);
        auto *selectorScroll=window.findChild<QScrollArea *>("dictationSelectorScroll");QVERIFY(selectorScroll);
        QCOMPARE(selectorScroll->horizontalScrollBarPolicy(),Qt::ScrollBarAlwaysOff);
        QTRY_VERIFY_WITH_TIMEOUT(selectorScroll->verticalScrollBar()->maximum()>0,500);
        selectorScroll->verticalScrollBar()->setValue(selectorScroll->verticalScrollBar()->maximum());
        const QRect deviceBounds(window.m_device->mapTo(selectorScroll->viewport(),QPoint()),window.m_device->size());
        QVERIFY(deviceBounds.bottom()<=selectorScroll->viewport()->rect().bottom());
        auto *actions=window.findChild<QWidget *>("dictationActions");
        QRect bounds;
        for(QPushButton *button:{window.m_record,window.m_cancelStt,window.m_transcribeFile,static_cast<QPushButton *>(window.m_live)})bounds=bounds.united(QRect(button->mapTo(actions,QPoint()),button->size()));
        QVERIFY(qAbs(bounds.center().x()-actions->rect().center().x())<=1);
        // A wrapped editable selection can still be focused and edited normally.
        window.m_dictate->setChecked(false);
        const QString longModel="provider/"+QString("long-model-identifier-").repeated(6);
        window.m_model->setCurrentText(longModel);window.m_text->setFocus();QCoreApplication::processEvents();
        QCOMPARE(window.m_model->currentText(),longModel);
        QTRY_VERIFY_WITH_TIMEOUT(window.m_model->height()>=window.m_model->heightForWidth(window.m_model->width()),500);
        QVERIFY(!window.m_model->lineEdit()->isVisible());
        QTest::mouseClick(window.m_model,Qt::LeftButton,Qt::NoModifier,QPoint(15,window.m_model->height()/2));
        QCoreApplication::processEvents();QVERIFY(window.m_model->lineEdit()->isVisible());
        QVERIFY(window.m_model->lineEdit()->hasFocus());
        QTest::keyClick(window.m_model->lineEdit(),Qt::Key_End);QTest::keyClicks(window.m_model->lineEdit(),"-edited");
        window.m_text->setFocus();QCoreApplication::processEvents();
        QCOMPARE(window.m_model->currentText(),longModel+"-edited");
        QVERIFY(!window.m_model->lineEdit()->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(window.m_model->height()>=window.m_model->heightForWidth(window.m_model->width()),500);
    }
    void lockingOneProfilePreservesOtherSessionKeys() {
        MainWindow window;window.m_demo=true;
        auto first=serverProfileForProvider("openrouter");first["apiKey"]="first-test-key";
        auto second=serverProfileForProvider("elevenlabs");second["apiKey"]="second-test-key";
        window.m_settings["profiles"]=QJsonArray{first,second};window.updateProfileChoices();
        window.lockProfileCredential(first.value("id").toString());
        const auto profiles=window.m_settings.value("profiles").toArray();
        QVERIFY(profiles.first().toObject().value("apiKey").toString().isEmpty());
        QCOMPARE(profiles.last().toObject().value("apiKey").toString(),QString("second-test-key"));
    }
    void dictationReplacesOnlySelectionAndCreatesOneUndoStep() {
        MainWindow window; prepare(window);
        // UI lifecycle fixture: these are the real service signals, with
        // deterministic text instead of a physical microphone/inference run.
        QCOMPARE(window.findChildren<NumberedEditor *>().size(), 1);
        QVERIFY(window.findChildren<QTabWidget *>().isEmpty());
        const QString original = "Before selected after.";
        window.m_text->setPlainText(original);
        window.m_text->document()->setModified(false);
        QTextCursor cursor(window.m_text->document());
        cursor.setPosition(7); cursor.setPosition(15, QTextCursor::KeepAnchor);
        window.m_text->setTextCursor(cursor);
        QVERIFY(window.beginDictation());
        QVERIFY(window.m_text->isReadOnly());
        QVERIFY(!window.m_speak->isEnabled());
        window.m_stt.transcript("spoken", false);
        QCOMPARE(window.m_text->toPlainText(), QString("Before spoken after."));
        // A device refresh can finish after capture started; its failure must
        // not discard a running dictation or unlock the editor.
        window.m_stt.devicesError("Fixture device-discovery failure");
        QVERIFY(window.m_dictationActive);
        QVERIFY(window.m_text->isReadOnly());
        QCOMPARE(window.m_text->toPlainText(), QString("Before spoken after."));
        window.m_stt.transcript("spoken words", false);
        QCOMPARE(window.m_text->toPlainText(), QString("Before spoken words after."));
        window.m_stt.transcript("final words", true);
        window.m_stt.finished();
        QCOMPARE(window.m_text->toPlainText(), QString("Before final words after."));
        QVERIFY(!window.m_text->isReadOnly());
        QVERIFY(window.m_speak->isEnabled());
        QVERIFY(window.m_text->document()->isModified());
        window.m_stt.transcript("late result", true);
        QCOMPARE(window.m_text->toPlainText(), QString("Before final words after."));
        window.m_text->undo();
        QCOMPARE(window.m_text->toPlainText(), original);
        window.m_text->redo();
        QCOMPARE(window.m_text->toPlainText(), QString("Before final words after."));
    }
    void dictationInsertsAtCaret_data() {
        QTest::addColumn<QString>("original");
        QTest::addColumn<int>("position");
        QTest::addColumn<QString>("spoken");
        QTest::addColumn<QString>("expected");
        QTest::newRow("empty") << QString() << 0 << QString("Hello.") << QString("Hello.");
        QTest::newRow("before") << QString("after.") << 0 << QString("Before") << QString("Before after.");
        QTest::newRow("after") << QString("Before") << 6 << QString("after") << QString("Before after");
        QTest::newRow("middle") << QString("First last.") << 6 << QString("middle") << QString("First middle last.");
        QTest::newRow("paragraphs") << QString("\n\nsuffix") << 0 << QString("prefix") << QString("prefix\n\nsuffix");
        QTest::newRow("unicode") << QString::fromUtf8("Café 👋 test") << 8 << QString("spoken") << QString::fromUtf8("Café 👋 spoken test");
    }
    void dictationInsertsAtCaret() {
        QFETCH(QString, original); QFETCH(int, position);
        QFETCH(QString, spoken); QFETCH(QString, expected);
        MainWindow window; prepare(window);
        window.m_text->setPlainText(original);
        QTextCursor cursor(window.m_text->document()); cursor.setPosition(position);
        window.m_text->setTextCursor(cursor);
        QVERIFY(window.beginDictation());
        window.m_stt.transcript(spoken, true); window.m_stt.finished();
        QCOMPARE(window.m_text->toPlainText(), expected);
        QVERIFY(!window.m_text->isReadOnly());
    }
    void unsuccessfulDictationRestoresOriginalTextAndSelection_data() {
        QTest::addColumn<QString>("completion");
        QTest::newRow("cancel") << QString("cancel");
        QTest::newRow("escape") << QString("escape");
        QTest::newRow("error") << QString("error");
        QTest::newRow("no-speech") << QString("no-speech");
        QTest::newRow("finished-without-result") << QString("finished-without-result");
    }
    void unsuccessfulDictationRestoresOriginalTextAndSelection() {
        QFETCH(QString, completion);
        MainWindow window; prepare(window);
        const QString original = "Keep\nthis selection\nunchanged.";
        window.m_text->setPlainText(original);
        window.m_text->document()->setModified(false);
        QTextCursor cursor(window.m_text->document());
        cursor.setPosition(19); cursor.setPosition(5, QTextCursor::KeepAnchor);
        window.m_text->setTextCursor(cursor);
        QVERIFY(window.beginDictation());
        window.m_stt.transcript("provisional", false);
        QVERIFY(window.m_text->toPlainText() != original);
        if (completion == "cancel") window.cancelDictation();
        else if (completion == "escape") dispatchEscape(window);
        else if (completion == "error") { window.m_stt.error("Fixture recognition error"); window.m_stt.finished(); }
        else if (completion == "no-speech") { window.m_stt.transcript(QString(), true); window.m_stt.finished(); }
        else window.m_stt.finished();
        QCOMPARE(window.m_text->toPlainText(), original);
        QCOMPARE(window.m_text->textCursor().position(), 5);
        QCOMPARE(window.m_text->textCursor().anchor(), 19);
        QVERIFY(!window.m_text->document()->isModified());
        QVERIFY(!window.m_text->isReadOnly());
        QVERIFY(!window.m_sttBusy);
        QVERIFY(!window.m_recording);
        window.m_stt.transcript("stale final", true);
        QCOMPARE(window.m_text->toPlainText(), original);
    }
    void playbackAndDictationAreMutuallyExclusive() {
        MainWindow window; prepare(window, "/delay");
        const auto original = window.m_text->toPlainText();
        QVERIFY(window.beginDictation());
        window.startSpeech();
        QVERIFY(!window.m_active);
        QVERIFY(!window.m_speak->isEnabled());
        QVERIFY(!window.beginDictation());
        window.cancelDictation();
        window.startSpeech();
        QVERIFY(window.m_active);
        QVERIFY(!window.m_record->isEnabled());
        QVERIFY(!window.beginDictation());
        window.record();
        QVERIFY(!window.m_sttBusy);
        QCOMPARE(window.m_text->toPlainText(), original);
        window.stopSpeech();
        QVERIFY(window.m_record->isEnabled());
        QVERIFY(window.beginDictation());
        window.cancelDictation();
    }
    void openRouterVoicesFollowSelectedModel() {
        MainWindow window;
        auto profile=defaultServerProfile();profile["adapter"]="openrouter";
        profile["model"]="model-a";profile["voice"]="voice-a";
        profile["discovery"]=QJsonObject{{"models",QJsonArray{
            QJsonObject{{"id","model-a"},{"voices",QJsonArray{QJsonObject{{"id","voice-a"}}}}},
            QJsonObject{{"id","model-b"},{"voices",QJsonArray{QJsonObject{{"id","voice-b"}}}}}
        }}};
        window.m_settings["profiles"]=QJsonArray{profile};window.m_settings["selectedProfile"]=0;
        window.updateProfileChoices();
        QCOMPARE(window.m_voice->currentText(),QString("voice-a"));
        window.m_model->setCurrentText("model-b");
        QCOMPARE(window.m_voice->currentText(),QString("voice-b"));
        QCOMPARE(window.m_voice->count(),1);
        window.m_model->setCurrentText("manual-model");
        QVERIFY(window.m_voice->currentText().isEmpty());
        window.m_voice->setCurrentText("manual-voice");
        QCOMPARE(window.selectedProfile()["voice"].toString(),QString("manual-voice"));
    }
    void profileEditsSurviveSwitching() {
        MainWindow window; prepare(window);
        auto profiles = window.m_settings["profiles"].toArray();
        auto second = defaultServerProfile(); second["name"] = "Second";
        profiles.append(second); window.m_settings["profiles"] = profiles; window.updateProfileChoices();
        window.m_model->setCurrentText("saved-model"); window.m_voice->setCurrentText("saved-voice");
        window.m_server->setCurrentIndex(1); window.m_server->setCurrentIndex(0);
        QCOMPARE(window.m_model->currentText(), QString("saved-model"));
        QCOMPARE(window.m_voice->currentText(), QString("saved-voice"));
    }
};
QTEST_MAIN(UiFlowTest)
#include "ui_flow_test.moc"
