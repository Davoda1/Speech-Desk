#include <QGuiApplication>
#include <QSvgRenderer>
#include <QPainter>
int main(int argc,char **argv){qputenv("QT_QPA_PLATFORM","offscreen");QGuiApplication app(argc,argv);if(argc<3)return 1;QSvgRenderer r(QString::fromLocal8Bit(argv[1]));QImage img(256,256,QImage::Format_ARGB32_Premultiplied);img.fill(Qt::transparent);QPainter p(&img);r.render(&p);p.end();return img.save(QString::fromLocal8Bit(argv[2]))?0:2;}
