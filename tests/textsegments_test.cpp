#include <QtTest>
#include "textsegments.h"
class TextSegmentsTest:public QObject {
    Q_OBJECT
private slots:
    void offsetAndParagraphs(){QString t=" First sentence.\n\nSecond sentence!  ";auto s=splitSpeechText(t,500,12);QCOMPARE(s.size(),2);for(const auto &v:s)QCOMPARE(t.mid(v.start-12,v.length),v.text);QCOMPARE(s.first().start,13);QCOMPARE(s.last().text,QString("Second sentence!"));}
    void longTextCoverage(){QString t;for(int i=0;i<300;++i)t+="word ";auto s=splitSpeechText(t,80);QString combined;for(auto v:s){QVERIFY(v.length<=80);combined+=v.text+" ";}QCOMPARE(combined.simplified(),t.simplified());}
    void unicode(){QString t=QString(79,'a')+QString::fromUtf8("🙂🙂🙂")+QString(100,'b');auto s=splitSpeechText(t,80);QString combined;for(auto v:s){QVERIFY(!v.text.front().isLowSurrogate());QVERIFY(!v.text.back().isHighSurrogate());combined+=v.text;}QCOMPARE(combined,t);}
    void empty(){QVERIFY(splitSpeechText(" \n \t ").isEmpty());}
};
QTEST_APPLESS_MAIN(TextSegmentsTest)
#include "textsegments_test.moc"
