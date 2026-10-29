#include "numberededitor.h"
#include <QPainter>
#include <QTextBlock>
#include <QScrollBar>
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(NumberedEditor *editor):QWidget(editor),m_editor(editor) {}
    QSize sizeHint() const override { return {m_editor->gutterWidth(),0}; }
protected:
    void paintEvent(QPaintEvent *event) override { m_editor->paintGutter(event); }
private: NumberedEditor *m_editor;
};
NumberedEditor::NumberedEditor(QWidget *parent):QPlainTextEdit(parent),m_gutter(new LineNumberArea(this))
{
    setFrameShape(QFrame::NoFrame);
    setTabStopDistance(32);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    document()->setDocumentMargin(16);
    connect(this,&QPlainTextEdit::blockCountChanged,this,[this]{updateGutterWidth();});
    connect(this,&QPlainTextEdit::updateRequest,this,[this](const QRect &r,int dy){
        if(dy)m_gutter->scroll(0,dy);else m_gutter->update(0,r.y(),m_gutter->width(),r.height());
        if(r.contains(viewport()->rect()))updateGutterWidth();
    });
    updateGutterWidth();
    // Document layout initialization is not an edit to the user's text.
    document()->setModified(false);
}
int NumberedEditor::gutterWidth() const { return 26 + fontMetrics().horizontalAdvance('9') * QString::number(qMax(1,blockCount())).size(); }
void NumberedEditor::updateGutterWidth(){setViewportMargins(gutterWidth(),0,0,0);m_gutter->update();}
void NumberedEditor::resizeEvent(QResizeEvent *e){QPlainTextEdit::resizeEvent(e);const QRect r=contentsRect();m_gutter->setGeometry(r.left(),r.top(),gutterWidth(),r.height());}
void NumberedEditor::paintGutter(QPaintEvent *e)
{
    QPainter p(m_gutter);p.fillRect(e->rect(),palette().color(QPalette::AlternateBase));
    auto block=firstVisibleBlock();int number=block.blockNumber();
    int top=qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom=top+qRound(blockBoundingRect(block).height());
    p.setPen(palette().color(QPalette::PlaceholderText));
    while(block.isValid()&&top<=e->rect().bottom()){
        if(block.isVisible()&&bottom>=e->rect().top())p.drawText(0,top,m_gutter->width()-10,fontMetrics().height(),Qt::AlignRight,QString::number(number+1));
        block=block.next();top=bottom;bottom=top+qRound(blockBoundingRect(block).height());++number;
    }
}
void NumberedEditor::setReadingRange(int start,int length,int wordStart,int wordLength,bool follow)
{
    QList<QTextEdit::ExtraSelection> selections;
    if(start<0||length<=0){clearReadingRange();return;}
    auto selection=[this](int at,int count,QColor background,bool strong){
        QTextEdit::ExtraSelection s;s.cursor=QTextCursor(document());
        s.cursor.setPosition(qBound(0,at,document()->characterCount()-1));
        s.cursor.setPosition(qBound(0,at+count,document()->characterCount()-1),QTextCursor::KeepAnchor);
        s.format.setBackground(background);
        if(strong){s.format.setForeground(QColor("#ffffff"));s.format.setFontWeight(QFont::DemiBold);}
        return s;
    };
    bool dark=palette().color(QPalette::Base).lightness()<128;
    selections<<selection(start,length,QColor(dark?"#253d52":"#dceeff"),false);
    if(wordStart>=0&&wordLength>0)selections<<selection(wordStart,wordLength,QColor("#2875bf"),true);
    setExtraSelections(selections);
    QTextCursor c(document());c.setPosition(qBound(0,wordStart>=0?wordStart:start,document()->characterCount()-1));
    if(follow && (c.blockNumber()!=m_lastFollowBlock || !viewport()->rect().contains(cursorRect(c)))) {
        const auto saved=textCursor();setTextCursor(c);ensureCursorVisible();int v=verticalScrollBar()->value(),h=horizontalScrollBar()->value();setTextCursor(saved);verticalScrollBar()->setValue(v);horizontalScrollBar()->setValue(h);m_lastFollowBlock=c.blockNumber();
    }
}
void NumberedEditor::clearReadingRange(){setExtraSelections({});m_lastFollowBlock=-1;}
