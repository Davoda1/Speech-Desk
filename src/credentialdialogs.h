#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
class QWidget;

bool unlockCredentialForProfile(QWidget *parent,const QJsonObject &profile,
    const QString &directory,QString *apiKey,const QByteArray &staged={});
bool protectCredentialForProfile(QWidget *parent,const QJsonObject &profile,
    const QString &currentKey,bool replacement,QByteArray *encrypted,QString *apiKey);
